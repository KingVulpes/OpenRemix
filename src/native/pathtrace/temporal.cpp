#include "pathtrace/gpu.h"
#include "pathtrace/shaders/embedded.h"

namespace remix {
  using namespace trace::detail;
  void PathTracer::Gpu::destroyIndirect() {
    for (auto &pipeline : indirectPass.pipeline) {
      forgetLightingPipeline(pipeline);
      if (pipeline) vkDestroyPipeline(device, pipeline, nullptr);
      pipeline = VK_NULL_HANDLE;
    }
    indirectPass.results.reset();
    indirectPass.active = indirectPass.failed = false;
  }

  void PathTracer::Gpu::configureIndirect(Frame &frame, const TraceOptions &options, bool querySharc) {
    indirectPass.active = false;
    if (!indirectPass.mode || indirectPass.failed || !denoiser || !postSplit ||
        !splitLightingActive || !localIlluminationActive || !options.bounces || options.pbrLocalReuse ||
        (indirectPass.mode == 1 && (querySharc || options.separateLocalLighting != 3)))
      return;
    try {
      auto &pipeline = indirectPass.pipeline[querySharc];
      if (!pipeline) {
        const auto *code = querySharc ? trace::sharcIndirectTemporalShader : trace::indirectTemporalShader;
        const size_t bytes = querySharc ? sizeof(trace::sharcIndirectTemporalShader) : sizeof(trace::indirectTemporalShader);
        VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = bytes, .pCode = code};
        VkShaderModule module{};
        check(vkCreateShaderModule(device, &info, nullptr, &module));
        const VkBool32 guidance = VK_FALSE;
        const VkSpecializationMapEntry entry{1, 0, sizeof(guidance)};
        const VkSpecializationInfo specialization{1, &entry, sizeof(guidance), &guidance};
        VkComputePipelineCreateInfo create{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module,
                                                      .pName = "main", .pSpecializationInfo = &specialization},
                                            .layout = temporalLayout};
        const auto result = vkCreateComputePipelines(device, pipelineCache, 1, &create, nullptr, &pipeline);
        vkDestroyShaderModule(device, module, nullptr);
        check(result);
        lightingPipelines[pipeline] = {code, bytes, temporalLayout};
        // On the measured Turing driver, first-hit termination raises the
        // uncached producer from 128 to 168 registers. Local visibility only
        // returns a boolean, so closest-blocker traversal preserves transport.
        lightingPipelines[pipeline].preferFirstHit = querySharc;
      }
      const VkDeviceSize bytes = VkDeviceSize(extent.width) * extent.height * 16;
      if (!indirectPass.results || indirectPass.results->size < bytes) {
        auto next = buffer(bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        if (indirectPass.results) retiredBuffers.push_back(std::move(indirectPass.results));
        indirectPass.results = std::move(next);
      }
      frame.indirectResults = indirectPass.results->address;
      indirectPass.active = true;
    } catch (VkResult error) {
      if (error == VK_ERROR_DEVICE_LOST) throw;
      indirectPass.failed = true;
      std::fprintf(stderr, "Separate indirect pass unavailable (%d); using inline tracing\n", int(error));
    }
  }

  void PathTracer::Gpu::destroyPost() {
    if (denoiser) {
      auto shadows = denoiser->shadowStatistics();
      shadowFrames += shadows.renderedFrames;
      shadowResets += shadows.historyResets;
      auto s = denoiser->statistics();
      historyResets += s.historyResets;
      denoisedFrames += s.renderedFrames;
      postPeakMemory = std::max(postPeakMemory, s.peakMemoryBytes);
    }
    destroyIndirect();
    destroySharc(true);
    forgetLightingPipeline(temporalTransparencyCompute);
    forgetLightingPipeline(temporalCompute);
    forgetLightingPipeline(confidenceCompute);
    forgetLightingPipeline(splitDirectCompute);
    if (splitDirectCompute)
      vkDestroyPipeline(device, splitDirectCompute, nullptr);
    splitDirectCompute = VK_NULL_HANDLE;
    if (temporalTransparencyCompute)
      vkDestroyPipeline(device, temporalTransparencyCompute, nullptr);
    temporalTransparencyCompute = VK_NULL_HANDLE;
    if (temporalCompute)
      vkDestroyPipeline(device, temporalCompute, nullptr);
    if (confidenceCompute)
      vkDestroyPipeline(device, confidenceCompute, nullptr);
    if (temporalLayout)
      vkDestroyPipelineLayout(device, temporalLayout, nullptr);
    temporalCompute = confidenceCompute = VK_NULL_HANDLE;
    temporalLayout = VK_NULL_HANDLE;
    denoiser.reset();
    postAttempted = false;
    postFailure.clear();
  }
  void PathTracer::Gpu::beginFrame(const TraceOptions &options) {
    retiredPreviousVertices.clear(); // beginFrame runs after the previous submission fence.
    const auto &caps = postCapabilities;
    const bool reblur = options.denoiser && !options.accumulate && !options.sharcDebug && caps.reblurShSupported;
    const bool taa = options.taa && !upscaler && !options.accumulate && !options.sharcDebug && caps.taaSupported;
    bool sigma = options.sigma && !options.accumulate && !options.sharcDebug &&
                       sigmaCapable;
    bool local = localSeen && (reblur || taa || sigma || upscaler);
    const bool localLimited = local && !localCapable[sigma];
    if (localLimited) {
      // Raw local light must never be multiplied by the sun's shadow mask.
      sigma = false;
      local = localCapable[0];
    }
    const bool localDenoise = local && options.localDenoiser && reblur;
    const bool split = local && options.separateLocalLighting != 0 && denoise::localSupported(physical, sigma, true);
    const bool combinedDirect = split && options.separateLocalLighting == 3;
    if (reblur != postReblur || taa != postTaa || sigma != postSigmaRequested ||
        local != postLocalRequested || localDenoise != postLocalDenoiseRequested || split != postSplitRequested ||
        combinedDirect != postCombinedDirect) {
      check(vkDeviceWaitIdle(device));
      destroyPost();
      postReblur = reblur;
      postTaa = taa;
      postSigma = postSigmaRequested = sigma;
      postLocal = postLocalRequested = local;
      postLocalDenoise = postLocalDenoiseRequested = localDenoise;
      postSplit = postSplitRequested = split;
      // A combined signal cannot reuse a history that held only one family.
      postCombinedDirect = combinedDirect;
      history.clear();
    }
    if (!postAttempted)
      postFailure.clear();
    if (localLimited)
      postFailure = "Insufficient descriptors for separate local and sun signals; SIGMA is disabled";
    if (local && options.separateLocalLighting && !split)
      postFailure = options.separateLocalLighting == 3
          ? "Insufficient descriptors for a separate combined-light pass; inline combined lighting active"
          : "Insufficient descriptors for separate torch/emissive histories; combined local lighting active";
    if ((options.denoiser && !caps.reblurShSupported) || (options.taa && !caps.taaSupported))
      postFailure = caps.unavailableReason;
    if ((!reblur && !taa && !sigma && !upscaler) || postAttempted)
      return;
    postSigma = sigma;
    postLocal = local;
    postLocalDenoise = localDenoise;
    postSplit = split;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
      postAttempted = true;
      std::string failure;
      try {
        auto next = std::make_unique<denoise::Renderer>();
        next->initialize(physical, device, extent, reblur, queueFamily, postSigma, postLocal, postLocalDenoise, postSplit);
        VkDescriptorSetLayout layouts[] = {setLayout, next->inputLayout()};
        VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 2,
                                      .pSetLayouts = layouts};
        check(vkCreatePipelineLayout(device, &li, nullptr, &temporalLayout));
        auto make = [&](const uint32_t *code, size_t size, VkPipeline &out) {
          VkShaderModuleCreateInfo ci{
              .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
          VkShaderModule shader{};
          check(vkCreateShaderModule(device, &ci, nullptr, &shader));
          // Compile ordinary local lighting first; guidance is a lazy variant.
          const VkBool32 guidance = VK_FALSE;
          const VkSpecializationMapEntry guideEntry{1, 0, sizeof(guidance)};
          const VkSpecializationInfo noGuiding{1, &guideEntry, sizeof(guidance), &guidance};
          VkComputePipelineCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                         .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = shader,
                                                   .pName = "main", .pSpecializationInfo = &noGuiding},
                                         .layout = temporalLayout};
          auto result = vkCreateComputePipelines(device, pipelineCache, 1, &pi, nullptr, &out);
          vkDestroyShaderModule(device, shader, nullptr);
          check(result);
          lightingPipelines[out] = {code, size, temporalLayout};
          lightingPipelines[out].supportsGuiding = &out == &temporalCompute || &out == &splitDirectCompute;
        };
        if (postSplit && postSigma)
          make(trace::splitLocalSigmaTemporalShader, sizeof(trace::splitLocalSigmaTemporalShader), temporalCompute);
        else if (postSplit)
          make(trace::splitLocalTemporalShader, sizeof(trace::splitLocalTemporalShader), temporalCompute);
        else if (postLocal && postSigma)
          make(trace::localSigmaTemporalShader, sizeof(trace::localSigmaTemporalShader), temporalCompute);
        else if (postLocal)
          make(trace::localTemporalShader, sizeof(trace::localTemporalShader), temporalCompute);
        else if (postSigma)
          make(trace::sigmaTemporalShader, sizeof(trace::sigmaTemporalShader), temporalCompute);
        else
          make(trace::temporalShader, sizeof(trace::temporalShader), temporalCompute);
        make(trace::temporalTransparencyShader, sizeof(trace::temporalTransparencyShader),
             temporalTransparencyCompute);
        if (postSplit)
          make(trace::splitLocalDirectShader, sizeof(trace::splitLocalDirectShader), splitDirectCompute);
        if (reblur)
          make(trace::confidenceShader, sizeof(trace::confidenceShader), confidenceCompute);
        denoiser = std::move(next);
        return;
      } catch (VkResult r) {
        if (r == VK_ERROR_DEVICE_LOST)
          throw;
        failure = "Temporal resource initialization failed (Vulkan " + std::to_string(r) + ")";
      } catch (const std::exception &e) {
        failure = std::string("Temporal initialization failed: ") + e.what();
      }
      const bool retry = postSplit || postLocalDenoise || postLocal || postSigma;
      const bool dropSplit = postSplit;
      const bool dropDenoise = postLocalDenoise;
      destroyPost();
      if (dropSplit) {
        postSplit = false;
        postFailure = failure + "; combined local lighting active";
      } else if (dropDenoise) {
        postLocalDenoise = false;
        postFailure = failure + "; local lights use unfiltered radiance";
      } else {
        postLocal = postSigma = false;
        postSplit = false;
        postFailure = failure + (retry ? "; local and sun lighting are unfiltered" : "; raw tracing remains active");
      }
      postAttempted = true;
      if (!retry)
        break;
    }
  }

  void PathTracer::beginFrame(const TraceOptions &options, const RasterRenderer& resources) {
    auto& g = *m_gpu;
    const bool perspective = resources.cameraValid() && upscale::supportsCamera(
        scene::multiply(resources.camera(), scene::inverse(resources.view())));
    const bool fsr = options.fsr && !options.accumulate && !options.sharcDebug &&
                     g.postCapabilities.taaSupported && perspective;
    if (fsr != g.fsrRequested || (fsr && options.fsrQuality != g.fsrQuality)) {
      g.fsrRequested = fsr;
      g.fsrQuality = options.fsrQuality;
      g.createTarget();
    }
    if (options.fsr && !options.accumulate && !options.sharcDebug) {
      if (!g.postCapabilities.taaSupported) g.fsrFailure = g.postCapabilities.unavailableReason;
      else if (!perspective) g.fsrFailure = "FSR requires a finite perspective world camera; native resolution active";
    }
    m_gpu->localSeen |= resources.hasSphereLights();
    m_gpu->localSeen |= std::any_of(resources.draws().begin(), resources.draws().end(), [](const auto &draw) {
      const auto &e = draw.material->emission;
      return draw.traced() && (e[0] > 0 || e[1] > 0 || e[2] > 0);
    });
    m_gpu->beginFrame(options);
    if (g.upscaler && !g.denoiser) {
      const auto failure = g.postFailure;
      g.fsrRequested = false;
      g.createTarget();
      g.fsrRequested = true; // Retry on the next explicit mode/preset change.
      g.fsrFailure = failure + "; FSR requires temporal guide resources; native resolution active";
      g.beginFrame(options);
    }
    m_gpu->beginSharc(options);
    if (g.denoiser)
      g.denoiser->selectFrame(g.frameIndex);
    if (g.upscaler)
      g.upscaler->selectFrame(g.frameIndex);
  }
  upscale::Statistics PathTracer::upscalingStatistics() const {
    auto result = m_gpu->upscaler ? m_gpu->upscaler->statistics() : upscale::Statistics{};
    result.failure = m_gpu->fsrFailure;
    return result;
  }
  VkRenderPass PathTracer::skyPass() const { return m_gpu->skyPass; }
  void PathTracer::submitted() {
    auto &g = *m_gpu;
    if (!g.rendered)
      return;
    g.rendered = false;
    if (g.localGuidingActive) {
      g.lightGuiding.writeIndex ^= 1;
      g.lightGuiding.valid = true;
      ++g.lightGuiding.stats.renderedFrames;
    }
    if (g.pbrReuse.active) {
      g.pbrReuse.writeIndex ^= 1;
      g.pbrReuse.valid = true;
    }
    g.history.commit();
    if (g.sharc.stats.active && g.sharc.hasReceivers) {
      ++g.sharc.stats.renderedFrames;
      ++g.sharc.warmFrames;
    }
    g.retiredPreviousVertices = std::move(g.previousVertices);
    g.previousVertices = std::move(g.pendingVertices);
    // Preserve the submitted transform exactly. Inverting its rounded inverse
    // introduces false motion even for a stationary camera, especially far
    // from the world origin.
    g.previousCamera = g.pendingCamera;
    g.previousView = g.temporalFrame.view;
    g.previousProjection = g.pendingProjection;
    std::copy_n(g.temporalFrame.jitter, 2, g.previousJitter);
    g.previousSeed = uint32_t(g.sequence - 1);
    g.previousTime = g.pendingTime;
    std::copy_n(g.pendingSun, 9, g.previousSun);
    if (g.denoiser)
      g.denoiser->submitted();
  }
  remixapi_LocalLightStatisticsEXT PathTracer::localLightStatistics() const {
    const auto &g = *m_gpu;
    remixapi_LocalLightStatisticsEXT result{.structSize = sizeof(result)};
    result.lightCount = uint32_t(g.lightBvh.emitters().size());
    result.activeLights = g.lightBvh.active;
    result.nodeCount = uint32_t(g.lightBvh.nodes().size());
    result.treeDepth = g.lightBvh.depth;
    result.generation = g.lightBvh.generation;
    result.builds = g.lightBvh.builds;
    result.refits = g.lightBvh.refits;
    result.memoryBytes = (g.lightNodes ? g.lightNodes->size : 0) + (g.lightEmitters ? g.lightEmitters->size : 0);
    for (auto *buffer : {g.torchNodes.get(), g.torchEmitters.get(), g.emissiveNodes.get(), g.emissiveEmitters.get()})
      result.memoryBytes += buffer ? buffer->size : 0;
    result.cpuUpdateMilliseconds = g.lightBvh.updateMilliseconds;
    if (g.splitLightingActive)
      result.cpuUpdateMilliseconds += g.torchBvh.updateMilliseconds + g.emissiveBvh.updateMilliseconds;
    result.denoiserActive = g.denoiser && g.postLocalDenoise;
    if (g.denoiser) {
      result.gpuTimingsAvailable = g.denoiser->statistics().gpuTimingsAvailable;
      result.gpuDenoiseMilliseconds = g.denoiser->localMilliseconds();
    }
    std::strncpy(result.unavailableReason, g.postFailure.c_str(), sizeof(result.unavailableReason) - 1);
    return result;
  }
  remixapi_LocalLightGuidingStatisticsEXT PathTracer::localLightGuidingStatistics() const {
    return m_gpu->lightGuiding.stats;
  }
  remixapi_ShadowStatisticsEXT PathTracer::shadowStatistics() const {
    const auto &g = *m_gpu;
    auto result = g.denoiser ? g.denoiser->shadowStatistics()
                            : remixapi_ShadowStatisticsEXT{.structSize = sizeof(remixapi_ShadowStatisticsEXT)};
    result.supported = g.sigmaCapable;
    result.renderedFrames += g.shadowFrames;
    result.historyResets += g.shadowResets;
    result.sunAngularDiameterDegrees = g.previousSun[6];
    const char *reason = !result.supported ? "SIGMA is unavailable on this build/device" : g.postFailure.c_str();
    std::strncpy(result.unavailableReason, reason, sizeof(result.unavailableReason) - 1);
    return result;
  }
  remixapi_DenoisingStatisticsEXT PathTracer::denoisingStatistics() const {
    auto &g = *m_gpu;
    auto result =
        g.denoiser ? g.denoiser->statistics()
                   : remixapi_DenoisingStatisticsEXT{.structSize = sizeof(remixapi_DenoisingStatisticsEXT)};
    result.historyResets += g.historyResets;
    result.renderedFrames += g.denoisedFrames;
    result.peakMemoryBytes = std::max(result.peakMemoryBytes, g.postPeakMemory);
    std::strncpy(result.unavailableReason, g.postFailure.c_str(), sizeof(result.unavailableReason) - 1);
    return result;
  }
} // namespace remix
