#include "pathtrace/gpu.h"
#include "pathtrace/shaders/embedded.h"

namespace remix {
  using namespace trace::detail;
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
    destroySharc(true);
    forgetLightingPipeline(temporalTransparencyCompute);
    forgetLightingPipeline(temporalCompute);
    forgetLightingPipeline(confidenceCompute);
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
    const bool taa = options.taa && !options.accumulate && !options.sharcDebug && caps.taaSupported;
    bool sigma = options.sigma && !options.accumulate && !options.sharcDebug &&
                       sigmaCapable;
    bool local = localSeen && (reblur || taa || sigma);
    const bool localLimited = local && !localCapable[sigma];
    if (localLimited) {
      // Raw local light must never be multiplied by the sun's shadow mask.
      sigma = false;
      local = localCapable[0];
    }
    const bool localDenoise = local && options.localDenoiser && reblur;
    if (reblur != postReblur || taa != postTaa || sigma != postSigmaRequested ||
        local != postLocalRequested || localDenoise != postLocalDenoiseRequested) {
      destroyPost();
      postReblur = reblur;
      postTaa = taa;
      postSigma = postSigmaRequested = sigma;
      postLocal = postLocalRequested = local;
      postLocalDenoise = postLocalDenoiseRequested = localDenoise;
      history.clear();
    }
    if (!postAttempted)
      postFailure.clear();
    if (localLimited)
      postFailure = "Insufficient descriptors for separate local and sun signals; SIGMA is disabled";
    if ((options.denoiser && !caps.reblurShSupported) || (options.taa && !caps.taaSupported))
      postFailure = caps.unavailableReason;
    if ((!reblur && !taa && !sigma) || postAttempted)
      return;
    postSigma = sigma;
    postLocal = local;
    postLocalDenoise = localDenoise;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
      postAttempted = true;
      std::string failure;
      try {
        auto next = std::make_unique<denoise::Renderer>();
        next->initialize(physical, device, extent, reblur, queueFamily, postSigma, postLocal, postLocalDenoise);
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
          lightingPipelines[out].supportsGuiding = &out == &temporalCompute;
        };
        if (postLocal && postSigma)
          make(trace::localSigmaTemporalShader, sizeof(trace::localSigmaTemporalShader), temporalCompute);
        else if (postLocal)
          make(trace::localTemporalShader, sizeof(trace::localTemporalShader), temporalCompute);
        else if (postSigma)
          make(trace::sigmaTemporalShader, sizeof(trace::sigmaTemporalShader), temporalCompute);
        else
          make(trace::temporalShader, sizeof(trace::temporalShader), temporalCompute);
        make(trace::temporalTransparencyShader, sizeof(trace::temporalTransparencyShader),
             temporalTransparencyCompute);
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
      const bool retry = postLocalDenoise || postLocal || postSigma;
      const bool dropDenoise = postLocalDenoise;
      destroyPost();
      if (dropDenoise) {
        postLocalDenoise = false;
        postFailure = failure + "; local lights use unfiltered radiance";
      } else {
        postLocal = postSigma = false;
        postFailure = failure + (retry ? "; local and sun lighting are unfiltered" : "; raw tracing remains active");
      }
      postAttempted = true;
      if (!retry)
        break;
    }
  }

  void PathTracer::beginFrame(const TraceOptions &options, bool hasLocalLights) {
    m_gpu->localSeen |= hasLocalLights;
    m_gpu->beginFrame(options);
    m_gpu->beginSharc(options);
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
    g.history.commit();
    if (g.sharc.stats.active) {
      ++g.sharc.stats.renderedFrames;
      ++g.sharc.warmFrames;
    }
    g.retiredPreviousVertices = std::move(g.previousVertices);
    g.previousVertices = std::move(g.pendingVertices);
    g.previousCamera = scene::inverse(g.temporalFrame.inverseCamera);
    g.previousView = g.temporalFrame.view;
    g.previousProjection = g.pendingProjection;
    std::copy_n(g.temporalFrame.jitter, 2, g.previousJitter);
    g.previousSeed = uint32_t(g.sequence - 1);
    g.previousTime = g.pendingTime;
    std::copy_n(g.pendingSun, 8, g.previousSun);
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
    result.cpuUpdateMilliseconds = g.lightBvh.updateMilliseconds;
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
