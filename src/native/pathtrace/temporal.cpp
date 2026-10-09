#include "pathtrace/gpu.h"
#include "pathtrace/shader_code.h"
#include "runtime/pipelines.h"
#include "upscale/ngx.h"

namespace remix {
  using namespace trace::detail;
  void PathTracer::Gpu::destroyIndirect() {
    for (auto &pipeline : indirectPass.pipeline) {
      forgetLightingPipeline(pipeline);
      if (pipeline)
        pipeline::destroyPipeline(device, pipeline, nullptr);
      pipeline = VK_NULL_HANDLE;
    }
    indirectPass.results.reset();
    indirectPass.active = indirectPass.failed = false;
  }

  void PathTracer::Gpu::configureProfile(const TraceOptions &options) {
    // Isolated profiling can alternate dispatches without rebuilding the scene.
    // The fixture atomically replaces this file before requesting a cache reset.
    if (profileEveryFrame && indirectPass.modeFile && indirectPass.controlReset != options.sharcReset) {
      indirectPass.controlReset = options.sharcReset;
      if (auto *file = std::fopen(indirectPass.modeFile, "r")) {
        const int mode = std::fgetc(file);
        if (mode == '0' || mode == '1' || mode == 'a')
          indirectPass.mode = mode == '0' ? 0u : mode == '1' ? 2u : 1u;
        if (std::fgetc(file) == ',') {
          const int placement = std::fgetc(file);
          if (placement == '0' || placement == '1') deviceLocalScene = placement == '1';
        }
        std::fclose(file);
      }
    }
  }

  void PathTracer::Gpu::configureIndirect(Frame &frame, const TraceOptions &options, bool querySharc) {
    indirectPass.active = false;
    if (rrActive() || !indirectPass.mode || indirectPass.failed || !denoiser || !postSplit ||
        !splitLightingActive || !localIlluminationActive || !options.bounces || (options.pbrLocalReuse && options.localDirectClamp == 0 && options.indirectClamp == 0) ||
        (indirectPass.mode == 1 && (querySharc || options.separateLocalLighting != 3)))
      return;
    try {
      auto &pipeline = indirectPass.pipeline[querySharc];
      if (!pipeline) {
        const auto source = trace::shader(querySharc ? trace::Stage::sharcIndirectTemporal : trace::Stage::indirectTemporal);
        VkShaderModuleCreateInfo info{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = source.bytes, .pCode = source.code};
        VkShaderModule module{};
        check(pipeline::createShaderModule(device, &info, nullptr, &module));
        const VkBool32 guidance = VK_FALSE;
        const VkSpecializationMapEntry entry{1, 0, sizeof(guidance)};
        const VkSpecializationInfo specialization{1, &entry, sizeof(guidance), &guidance};
        VkComputePipelineCreateInfo create{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                            .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                      .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module,
                                                      .pName = "main", .pSpecializationInfo = &specialization},
                                            .layout = temporalLayout};
        const auto result = pipeline::createComputePipelines(
            device, VK_NULL_HANDLE, 1, &create, nullptr, &pipeline);
        pipeline::destroyShaderModule(device, module, nullptr);
        if (result == VK_NOT_READY)
          return;
        check(result);
        lightingPipelines[pipeline] = {source.code, source.bytes, temporalLayout};
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
    forgetLightingPipeline(frontGuidesCompute);
    if (frontGuidesCompute)
      pipeline::destroyPipeline(device, frontGuidesCompute, nullptr);
    frontGuidesCompute = VK_NULL_HANDLE;
    forgetLightingPipeline(psrCompute);
    if (psrCompute)
      pipeline::destroyPipeline(device, psrCompute, nullptr);
    psrCompute = VK_NULL_HANDLE;
    forgetLightingPipeline(singleTemporalTransparencyCompute);
    if (singleTemporalTransparencyCompute)
      pipeline::destroyPipeline(device, singleTemporalTransparencyCompute, nullptr);
    singleTemporalTransparencyCompute = VK_NULL_HANDLE;
    forgetLightingPipeline(temporalTransparencyCompute);
    forgetLightingPipeline(temporalCompute);
    forgetLightingPipeline(confidenceCompute);
    forgetLightingPipeline(splitDirectCompute);
    for (auto *pipeline : {&restir.initial, &restir.spatial}) {
      forgetLightingPipeline(*pipeline);
      if (*pipeline)
        pipeline::destroyPipeline(device, *pipeline, nullptr);
      *pipeline = VK_NULL_HANDLE;
    }
    restir.active = restir.historyValid = false;
    if (splitDirectCompute)
      pipeline::destroyPipeline(device, splitDirectCompute, nullptr);
    splitDirectCompute = VK_NULL_HANDLE;
    if (temporalTransparencyCompute)
      pipeline::destroyPipeline(device, temporalTransparencyCompute, nullptr);
    temporalTransparencyCompute = VK_NULL_HANDLE;
    if (temporalCompute)
      pipeline::destroyPipeline(device, temporalCompute, nullptr);
    if (confidenceCompute)
      pipeline::destroyPipeline(device, confidenceCompute, nullptr);
    if (temporalLayout)
      pipeline::destroyPipelineLayout(device, temporalLayout, nullptr);
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
    bool local = !rrActive() && localSeen && (reblur || taa || sigma || upscaler);
    const bool localLimited = local && !localCapable[sigma];
    if (localLimited) {
      // Raw local light must never be multiplied by the sun's shadow mask.
      sigma = false;
      local = localCapable[0];
    }
    const bool localDenoise = local && options.localDenoiser && reblur;
    const bool split = local && options.separateLocalLighting != 0 && (emissiveSeen || options.restir) &&
                       denoise::localSupported(physical, sigma, true);
    const bool combinedDirect = split && options.separateLocalLighting == 3;
    if (reblur != postReblur || taa != postTaa || sigma != postSigmaRequested ||
        local != postLocalRequested || localDenoise != postLocalDenoiseRequested || split != postSplitRequested ||
        combinedDirect != postCombinedDirect) {
      if (!preparing)
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
    if (local && options.separateLocalLighting && (emissiveSeen || options.restir) && !split)
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
        next->initialize(physical, device, extent, reblur, queueFamily,
                         postSigma, postLocal, postLocalDenoise, postSplit,
                         rrActive(), options.validation,
                         options.separateLocalLighting == 1,
                         rrActive() && caps.reblurShSupported);
        VkDescriptorSetLayout layouts[] = {setLayout, next->inputLayout()};
        VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 2,
                                      .pSetLayouts = layouts};
        check(pipeline::createPipelineLayout(device, &li, nullptr,
                                             &temporalLayout));
        // One batch: the temporal lighting passes compile concurrently.
        std::vector<LightingRequest> requests;
        auto make = [&](trace::Stage stage, VkPipeline &out) {
          requests.push_back({stage, temporalLayout, &out, &out == &temporalCompute || &out == &splitDirectCompute});
        };
        if (postSplit && postSigma)
          make(trace::Stage::splitLocalSigmaTemporal, temporalCompute);
        else if (postSplit)
          make(trace::Stage::splitLocalTemporal, temporalCompute);
        else if (postLocal && postSigma)
          make(trace::Stage::localSigmaTemporal, temporalCompute);
        else if (postLocal)
          make(trace::Stage::localTemporal, temporalCompute);
        else if (postSigma)
          make(trace::Stage::sigmaTemporal, temporalCompute);
        else
          make(trace::Stage::temporal, temporalCompute);
        make(trace::Stage::temporalTransparency,
             temporalTransparencyCompute);
        make(trace::Stage::frontGuides, frontGuidesCompute);
        make(trace::Stage::psr, psrCompute);
        make(trace::Stage::singleTemporalTransparency,
             singleTemporalTransparencyCompute);
        if (postSplit)
          make(trace::Stage::splitLocalDirect, splitDirectCompute);
        if (postSplit && options.restir) {
          requests.push_back({trace::Stage::splitRestirInitial, temporalLayout, &restir.initial, false});
          requests.push_back({trace::Stage::splitRestirSpatial, temporalLayout, &restir.spatial, false});
        }
        if (reblur)
          make(trace::Stage::confidence, confidenceCompute);
        createLightingPipelines(requests);
        denoiser = std::move(next);
        return;
      } catch (VkResult r) {
        if (preparing)
          throw;
        if (r == VK_ERROR_DEVICE_LOST)
          throw;
        failure = "Temporal resource initialization failed (Vulkan " + std::to_string(r) + ")";
      } catch (const std::exception &e) {
        if (preparing)
          throw;
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

  void PathTracer::setNgx(std::shared_ptr<upscale::Ngx> ngx) { m_gpu->ngx = std::move(ngx); }
  upscale::Backend PathTracer::Gpu::selectBackend() const {
    auto allowed = [&](upscale::Backend b) { return !(failedBackends & (1u << uint32_t(b))); };
    if (requestedBackend == upscale::Backend::Fsr31)
      return allowed(requestedBackend) ? requestedBackend : upscale::Backend::Native;
    if (requestedBackend == upscale::Backend::Native || !ngx) return upscale::Backend::Native;
    const auto caps = ngx->capabilities();
    if (rrRequested && caps.rr && allowed(upscale::Backend::DlssRr)) return upscale::Backend::DlssRr;
    if (caps.sr && allowed(upscale::Backend::DlssSr)) return upscale::Backend::DlssSr;
    return upscale::Backend::Native;
  }
  TraceOptions PathTracer::Gpu::effectiveOptions(TraceOptions options) const {
    if (rrActive()) {
      options.denoiser = options.localDenoiser = options.sigma = options.taa = false;
      options.blueNoiseShadows = options.blueNoiseLocalLights = false;
      options.waterIndirectReuse = options.pbrLocalReuse = false;
      // The shared family budget remains a combined budget. Per-family mode
      // retains both configured budgets and samples both families at every pixel.
      if (options.separateLocalLighting == 1 || options.separateLocalLighting == 3)
        options.separateLocalLighting = 0;
    }
    return options;
  }
  void PathTracer::prepareConfiguration(const TraceOptions &options,
                                        bool perspective, bool local,
                                        uint32_t failedBackends,
                                        const std::string &failure) {
    auto &g = *m_gpu;
    g.preparedPerspective = perspective;
    g.preparedLocal = local;
    g.requestedUpscaler = options.upscaler;
    g.fsrQuality = options.upscaler == upscale::Mode::Fsr31
                       ? options.fsrQuality
                       : options.dlssQuality;
    g.rrRequested = options.dlssRayReconstruction;
    g.failedBackends = failedBackends;
    g.fsrFailure = failure;
    // Schedule every pass's lighting pipelines before awaiting any of them.
    g.deferLighting = true;
    struct Defer {
      Gpu &g;
      ~Defer() { g.deferLighting = false; }
    } defer{g};
    g.configureFrame(options, perspective, local);
    g.finishLightingPipelines();
  }
  bool PathTracer::finishPreparation(VkQueue queue) {
    auto &g = *m_gpu;
    if (g.upscaler && !g.upscaler->finishInitialization(queue))
      return false;
    g.preparing = false;
    return true;
  }
  void PathTracer::beginFrame(const TraceOptions &options,
                              const RasterRenderer &) {
    auto &g = *m_gpu;
    g.configureFrame(options, g.preparedPerspective, g.preparedLocal);
  }
  void PathTracer::Gpu::configureFrame(const TraceOptions &options,
                                       bool perspective, bool local) {
    auto &g = *this;
    const bool eligible = !options.accumulate && !options.sharcDebug && g.postCapabilities.taaSupported && perspective;
    const auto backend = !eligible || options.upscaler == upscale::Mode::Off ? upscale::Backend::Native :
        options.upscaler == upscale::Mode::Fsr31 ? upscale::Backend::Fsr31 : upscale::Backend::DlssSr;
    const auto quality = options.upscaler == upscale::Mode::Fsr31 ? options.fsrQuality : options.dlssQuality;
    bool changed = options.upscaler != g.requestedUpscaler || quality != g.fsrQuality || options.dlssRayReconstruction != g.rrRequested;
    if (changed) { g.failedBackends = 0; g.fsrFailure.clear(); }
    changed |= backend != g.requestedBackend;
    g.requestedUpscaler = options.upscaler; g.requestedBackend = backend;
    g.fsrQuality = quality; g.rrRequested = options.dlssRayReconstruction;
    if (changed || !g.extent.width)
      g.createTarget();
    if (options.upscaler != upscale::Mode::Off && !options.accumulate && !options.sharcDebug) {
      if (!g.postCapabilities.taaSupported) g.fsrFailure = g.postCapabilities.unavailableReason;
      else if (!perspective) g.fsrFailure = "Reconstruction requires a finite perspective world camera";
      else if (!g.upscaler && g.fsrFailure.empty() && g.ngx) g.fsrFailure = g.ngx->capabilities().reason;
      else if (g.upscaler && options.dlssRayReconstruction && g.upscaler->statistics().backend == upscale::Backend::DlssSr && g.fsrFailure.empty())
        g.fsrFailure = g.ngx->capabilities().reason;
    }
    g.localSeen |= local;
    g.beginFrame(g.effectiveOptions(options));
    if (g.upscaler && !g.denoiser) {
      const auto failure = g.postFailure;
      g.failedBackends = ~0u;
      g.createTarget();
      g.fsrFailure = failure + "; reconstruction guide resources unavailable";
      g.beginFrame(options);
    }
    if (!g.denoiser)
      g.createRawPipelines();
    g.beginSharc(g.effectiveOptions(options));
    if (g.denoiser) g.denoiser->selectFrame(g.frameIndex);
    if (g.upscaler) g.upscaler->selectFrame(g.frameIndex);
  }
  upscale::Statistics PathTracer::upscalingStatistics() const {
    const auto &g = *m_gpu;
    auto result = g.upscaler ? g.upscaler->statistics() : upscale::Statistics{};
    result.requested = g.requestedUpscaler;
    result.render = g.extent; result.display = g.displayExtent;
    if (g.ngx) {
      auto caps = g.ngx->capabilities();
      result.srSupported = caps.sr; result.rrSupported = caps.rr; result.sdkAvailable = caps.sdk;
    }
    result.failures += g.reconstructionFailures;
    result.frames += g.reconstructedFrames; result.resets += g.reconstructionResets;
    if (result.failure.empty()) result.failure = g.fsrFailure;
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
    // The static set's history: this prepared frame is now the committed one.
    g.statics.committed(g.prepareSerial, g.retiredStates);
    if (g.upscaler && g.reconstructionWarmFrames < 128)
      ++g.reconstructionWarmFrames;
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
    // Visibility culling: lights with a cluster (low 16 bits) and lights
    // unrestricted by it (high 16 bits); 0 when no partition is active.
    if (g.lightVisibility.table())
      result.reserved = std::min(g.lightVisibility.clustered, 0xffffu) |
                        std::min(g.lightVisibility.unrestricted, 0xffffu) << 16;
    result.memoryBytes += (g.cameraCulling.nodes ? g.cameraCulling.nodes->size : 0) +
                          (g.cameraCulling.leaves ? g.cameraCulling.leaves->size : 0);
    result.memoryBytes += (g.visibilityNodes ? g.visibilityNodes->size : 0) +
                          (g.visibilityLeaves ? g.visibilityLeaves->size : 0) +
                          (g.visibilityTable ? g.visibilityTable->size : 0);
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
  remixapi_CameraCullingStatisticsEXT PathTracer::cameraCullingStatistics() const {
    return m_gpu->cameraCulling.statistics;
  }
  remixapi_LocalLightSetStatisticsEXT PathTracer::localLightSetStatistics() const {
    const auto &g = *m_gpu;
    const auto &tree = g.lightBvh;
    const auto &table = g.lightVisibility;
    remixapi_LocalLightSetStatisticsEXT result{.structSize = sizeof(result)};
    result.lastBuildReason = tree.lastBuildReason;
    result.staticLights = tree.staticLights;
    result.dynamicLights = tree.dynamicLights;
    result.retiredSlots = tree.retiredSlots;
    result.dynamicInstanceLights = tree.instanceLights;
    result.emittingInstances = g.emittingInstances;
    result.dynamicEmittingInstances = g.dynamicEmittingInstances;
    result.builds = tree.builds;
    result.refits = tree.refits;
    result.dynamicBuilds = tree.dynamicBuilds;
    static_assert(REMIXAPI_LOCAL_LIGHT_BUILD_REASONS_EXT == scene::LightBvh::ReasonCount);
    std::copy_n(tree.buildReasons, scene::LightBvh::ReasonCount, result.buildsByReason);
    if (table.table()) {
      result.tableBytes = table.table()->size() * sizeof(uint32_t);
      result.tableNodes = table.tableNodes();
    }
    result.tableBuilds = table.tableBuilds;
    result.tableUploads = g.visibilityTableUploads;
    result.tableMilliseconds = table.tableMilliseconds;
    result.tableTotalMilliseconds = table.tableTotalMilliseconds;
    result.buildMilliseconds = tree.buildMilliseconds;
    result.buildTotalMilliseconds = tree.buildTotalMilliseconds;
    result.updateMilliseconds = g.lightUpdateMilliseconds;
    result.updatePeakMilliseconds = std::max(g.lightUpdatePeak[0], g.lightUpdatePeak[1]);
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
  bool PathTracer::translucencyDenoiserActive() const {
    return m_gpu->denoiser && m_gpu->denoiser->foregroundActive();
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
