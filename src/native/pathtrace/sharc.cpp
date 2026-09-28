#include "pathtrace/gpu.h"
#include "pathtrace/shader_code.h"
#include "pathtrace/sharc_policy.h"
#include "runtime/pipelines.h"
#include <future>
#include <utility>

namespace remix {
  using namespace trace::detail;

  void PathTracer::Gpu::sharcPipeline(trace::Stage stage, VkPipelineLayout pipelineLayout, VkPipeline &out) {
    createLightingPipelines({{stage, pipelineLayout, &out, &out == &sharc.query || &out == &sharc.temporal,
                              &out != &sharc.update}});
  }

  void PathTracer::Gpu::createLightingPipelines(const std::vector<LightingRequest> &batch) {
    // Pipelines that already exist are kept.
    std::vector<LightingRequest> requests;
    for (const auto &request : batch)
      if (!*request.out)
        requests.push_back(request);
    if (requests.empty())
      return;
    // Compile ordinary local lighting first; guidance is a lazy variant.
    const VkBool32 guidance = VK_FALSE;
    const VkSpecializationMapEntry guideEntry{1, 0, sizeof(guidance)};
    const VkSpecializationInfo noGuiding{1, &guideEntry, sizeof(guidance), &guidance};
    // Embedded stages decompress on first use; decode the batch concurrently.
    std::vector<std::future<trace::ShaderCode>> decoding;
    for (const auto &request : requests)
      decoding.push_back(std::async(std::launch::async, trace::shader, request.stage));
    std::vector<trace::ShaderCode> codes;
    for (auto &code : decoding)
      codes.push_back(code.get());
    std::vector<VkShaderModule> modules(requests.size());
    std::vector<VkComputePipelineCreateInfo> infos(requests.size());
    std::vector<VkPipeline> pipelines(requests.size());
    struct Release {
      VkDevice device;
      std::vector<VkShaderModule> &modules;
      ~Release() {
        for (auto module : modules)
          if (module)
            pipeline::destroyShaderModule(device, module, nullptr);
      }
    } release{device, modules};
    for (size_t i = 0; i < requests.size(); ++i) {
      VkShaderModuleCreateInfo ci{
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = codes[i].bytes, .pCode = codes[i].code};
      check(pipeline::createShaderModule(device, &ci, nullptr, &modules[i]));
      infos[i] = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                  .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                            .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = modules[i], .pName = "main",
                            .pSpecializationInfo = &noGuiding},
                  .layout = requests[i].layout};
    }
    if (deferLighting) {
      pipeline::prefetchComputePipelines(device, uint32_t(infos.size()), infos.data());
      deferredLighting.insert(deferredLighting.end(), requests.begin(), requests.end());
      return;
    }
    const auto result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, uint32_t(infos.size()),
                                                         infos.data(), nullptr, pipelines.data());
    // Publish every pipeline that compiled, so failure cleanup releases it.
    for (size_t i = 0; i < requests.size(); ++i) {
      const auto &request = requests[i];
      *request.out = pipelines[i];
      if (!pipelines[i])
        continue;
      auto &entry = lightingPipelines[pipelines[i]];
      entry = {codes[i].code, codes[i].bytes, request.layout};
      entry.supportsGuiding = request.supportsGuiding;
      entry.supportsRoulette = request.supportsRoulette;
    }
    check(result);
  }

  void PathTracer::Gpu::finishLightingPipelines() {
    deferLighting = false;
    createLightingPipelines(std::exchange(deferredLighting, {}));
  }

  void PathTracer::Gpu::destroySharc(bool temporalOnly) {
    for (auto *pipeline : {&sharc.temporal, &sharc.temporalTransparency, &sharc.singleTemporalTransparency, &sharc.confidence}) {
      forgetLightingPipeline(*pipeline);
      if (*pipeline)
        pipeline::destroyPipeline(device, *pipeline, nullptr);
      *pipeline = VK_NULL_HANDLE;
    }
    if (temporalOnly)
      return;
    for (auto &frame : frames) {
      frame.sharcReadback.reset();
      if (frame.sharcTimestamps)
        vkDestroyQueryPool(device, frame.sharcTimestamps, nullptr);
      frame.sharcTimestamps = VK_NULL_HANDLE;
      frame.sharcPending = false;
    }
    for (auto *pipeline : {&sharc.update, &sharc.resolve, &sharc.query, &sharc.transparency, &sharc.singleTransparency}) {
      forgetLightingPipeline(*pipeline);
      if (*pipeline)
        pipeline::destroyPipeline(device, *pipeline, nullptr);
      *pipeline = VK_NULL_HANDLE;
    }
    if (sharc.timestamps)
      vkDestroyQueryPool(device, sharc.timestamps, nullptr);
    sharc.timestamps = VK_NULL_HANDLE;
    sharc.hash.reset();
    sharc.accumulation.reset();
    sharc.resolved.reset();
    sharc.counters.reset();
    sharc.readback.reset();
    sharc.clear = true;
    sharc.pending = false;
    sharc.hasReceivers = false;
    sharc.warmFrames = 0;
    sharc.stats.active = sharc.stats.capacity = sharc.stats.occupiedEntries = 0;
    sharc.stats.memoryBytes = 0;
  }

  void PathTracer::Gpu::beginSharc(const TraceOptions &options) {
    auto &s = sharc.stats;
    s.requested = options.sharc;
    s.debugView = options.sharcDebug;
    sharc.warmTarget = 2 * options.sharcAccumulationFrames;
    if (sharc.pending) {
      // beginFrame runs only after this slot's submission fence. Host memory
      // need not be coherent, so invalidate it after the GPU-to-host dependency.
      void *mapped{};
      check(vkMapMemory(device, sharc.readback->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
      VkMappedMemoryRange range{.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                .memory = sharc.readback->memory, .size = VK_WHOLE_SIZE};
      auto result = vkInvalidateMappedMemoryRanges(device, 1, &range);
      if (result == VK_SUCCESS) {
        uint32_t counters[4];
        std::memcpy(counters, mapped, sizeof(counters));
        s.updatePaths = counters[0];
        s.queryAttempts = counters[1];
        s.queryHits = counters[2];
        s.occupiedEntries = counters[3];
      }
      vkUnmapMemory(device, sharc.readback->memory);
      check(result);
      s.gpuTimingsAvailable = 0;
      if (sharc.timestamps) {
        uint64_t values[4][2]{};
        result = vkGetQueryPoolResults(device, sharc.timestamps, 0, 4, sizeof(values), values,
                                       sizeof(values[0]), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        if (result == VK_SUCCESS && values[0][1] && values[1][1] && values[2][1] && values[3][1]) {
          const uint64_t mask = timestampBits == 64 ? UINT64_MAX : ((uint64_t(1) << timestampBits) - 1);
          const double scale = properties.limits.timestampPeriod / 1e6;
          s.gpuUpdateMilliseconds = double((values[1][0] - values[0][0]) & mask) * scale;
          s.gpuResolveMilliseconds = double((values[3][0] - values[2][0]) & mask) * scale;
          s.gpuTimingsAvailable = 1;
        } else if (result != VK_NOT_READY)
          check(result);
      }
      sharc.pending = false;
    }
    s.active = 0;
#if REMIX_HAS_SHARC
    if (!sharc.attempted) {
      sharc.attempted = true;
      VkPhysicalDeviceVulkan12Features v12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
      VkPhysicalDeviceVulkan11Features v11{
          .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v12};
      VkPhysicalDeviceFeatures2 features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v11};
      vkGetPhysicalDeviceFeatures2(physical, &features);
      s.supported = v11.storageBuffer16BitAccess && v12.shaderFloat16 && v12.shaderBufferInt64Atomics;
      std::strncpy(s.unavailableReason, s.supported ? "" : "SHARC requires storageBuffer16BitAccess, shaderFloat16 and shaderBufferInt64Atomics; uncached tracing is active",
                   sizeof(s.unavailableReason) - 1);
    }
    if (!options.sharc || options.accumulate || !options.bounces) {
      sharc.clear = true;
      sharc.warmFrames = 0;
      sharc.attempted = false; // An explicit toggle allows retry after allocation/pipeline failure.
      return;
    }
    if (!s.supported || s.unavailableReason[0])
      return;
    try {
      const uint32_t capacity = 1u << options.sharcCapacityLog2;
      if (s.capacity != capacity) {
        if (!preparing)
          check(vkDeviceWaitIdle(device));
        destroySharc();
        constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        sharc.hash = buffer(uint64_t(capacity) * 8, usage);
        sharc.accumulation = buffer(uint64_t(capacity) * REMIX_SHARC_ACCUMULATION_STRIDE, usage);
        sharc.resolved = buffer(uint64_t(capacity) * REMIX_SHARC_RESOLVED_STRIDE, usage);
        sharc.counters = buffer(16, usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        sharc.readback = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        s.capacity = capacity;
        s.memoryBytes = sharc.hash->size + sharc.accumulation->size + sharc.resolved->size +
                        sharc.counters->size + sharc.readback->size;
      }
      // One batch: the SHARC passes compile concurrently.
      std::vector<LightingRequest> requests;
      auto add = [&](trace::Stage stage, VkPipelineLayout pipelineLayout, VkPipeline &out) {
        requests.push_back({stage, pipelineLayout, &out, &out == &sharc.query || &out == &sharc.temporal,
                            &out != &sharc.update});
      };
      add(trace::Stage::sharcUpdate, layout, sharc.update);
      add(trace::Stage::sharcResolve, layout, sharc.resolve);
      if (!denoiser)
        add(trace::Stage::sharcQuery, layout, sharc.query);
      if (!denoiser)
        add(trace::Stage::sharcTransparency, layout, sharc.transparency);
      add(trace::Stage::sharcSingleTransparency, layout,
          sharc.singleTransparency);
      if (denoiser) {
        if (postSplit && postSigma)
          add(trace::Stage::sharcSplitLocalSigmaTemporal, temporalLayout, sharc.temporal);
        else if (postSplit)
          add(trace::Stage::sharcSplitLocalTemporal, temporalLayout, sharc.temporal);
        else if (postLocal && postSigma)
          add(trace::Stage::sharcLocalSigmaTemporal, temporalLayout, sharc.temporal);
        else if (postLocal)
          add(trace::Stage::sharcLocalTemporal, temporalLayout, sharc.temporal);
        else if (postSigma)
          add(trace::Stage::sharcSigmaTemporal, temporalLayout, sharc.temporal);
        else
          add(trace::Stage::sharcTemporal, temporalLayout, sharc.temporal);
        add(trace::Stage::sharcTemporalTransparency, temporalLayout,
            sharc.temporalTransparency);
        add(trace::Stage::sharcSingleTemporalTransparency,
            temporalLayout, sharc.singleTemporalTransparency);
        if (postReblur)
          add(trace::Stage::sharcConfidence, temporalLayout, sharc.confidence);
      }
      createLightingPipelines(requests);
      if (!sharc.timestamps && timestamps) {
        VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4};
        check(vkCreateQueryPool(device, &qi, nullptr, &sharc.timestamps));
      }
      s.active = 1;
    } catch (VkResult r) {
      if (preparing)
        throw;
      if (r == VK_ERROR_DEVICE_LOST)
        throw;
      if (!preparing)
        check(vkDeviceWaitIdle(device));
      destroySharc();
      const auto reason = "SHARC initialization failed (Vulkan " + std::to_string(r) + "); uncached tracing is active";
      std::strncpy(s.unavailableReason, reason.c_str(), sizeof(s.unavailableReason) - 1);
    } catch (const std::exception &e) {
      if (preparing)
        throw;
      if (!preparing)
        check(vkDeviceWaitIdle(device));
      destroySharc();
      const auto reason = std::string("SHARC initialization failed: ") + e.what();
      std::strncpy(s.unavailableReason, reason.c_str(), sizeof(s.unavailableReason) - 1);
    }
#else
    std::strncpy(s.unavailableReason, "SHARC was disabled at build time; uncached tracing is active",
                 sizeof(s.unavailableReason) - 1);
#endif
  }

  void PathTracer::Gpu::configureSharc(Frame &frame, const TraceOptions &options, scene::TemporalEpoch currentEpoch,
                                      const std::vector<uint8_t> &geometry, uint64_t geometryGeneration,
                                      const std::vector<uint8_t> &dynamicGeometry, bool hasReceivers) {
    if (!sharc.stats.active)
      return;
    // Eligibility covers every traced surface, including off-screen receivers.
    // Idle scenes use the ordinary uncached shader; confidence tracing continues.
    sharc.clear |= !sharc.hasReceivers || !hasReceivers;
    sharc.hasReceivers = hasReceivers;
    if (!hasReceivers) {
      auto &s = sharc.stats;
      s.updatePaths = s.queryAttempts = s.queryHits = s.occupiedEntries = 0;
      s.gpuUpdateMilliseconds = s.gpuResolveMilliseconds = 0;
      s.gpuTimingsAvailable = 0;
    }
    // The static prefix geometry already held is equal without a compare.
    const bool sameGeometry = geometryGeneration && geometryGeneration == sharc.geometryGeneration;
    const bool geometryChanged = !sameGeometry && sharc.geometry != geometry;
    sharc.clear |= sharc.epoch != currentEpoch || sharc.reset != options.sharcReset ||
                   sharc.scale != options.sharcSceneScale || sharc.bounces != options.bounces || geometryChanged;
    sharc.epoch = currentEpoch;
    sharc.reset = options.sharcReset;
    sharc.scale = options.sharcSceneScale;
    sharc.bounces = options.bounces;
    if (geometryChanged)
      sharc.geometry = geometry;
    sharc.geometryGeneration = geometryGeneration;
    float light[9];
    std::copy_n(frame.direction, 3, light);
    std::copy_n(frame.radiance, 3, light + 3);
    std::copy_n(frame.skyRadiance, 3, light + 6);
    float oldEnergy = 0, newEnergy = 0, difference = 0, angle = 0;
    for (int i = 0; i < 3; ++i)
      angle += light[i] * sharc.light[i];
    for (int i = 3; i < 9; ++i) {
      oldEnergy += sharc.light[i];
      newEnergy += light[i];
      difference += std::abs(light[i] - sharc.light[i]);
    }
    // Large lighting cuts discard stale radiance, independently of NRD/TAA.
    // Ordinary time/weather updates shorten only the cache's averaging window.
    sharc.clear |= difference > .25f * std::max(oldEnergy, newEnergy) ||
                   (std::max(oldEnergy, newEnergy) > 0 && angle < .999f);
    const bool localChanged = !std::equal(std::begin(frame.localConfig), std::end(frame.localConfig), sharc.localConfig) ||
                              frame.localScale[0] != sharc.localIntensity || frame.localScale[1] != sharc.localRange;
    if (localChanged) {
      sharc.warmFrames = 0;
      // Complete darkness must not reuse cached torch radiance.
      sharc.clear |= sharc.localConfig[0] != 0 && frame.localConfig[0] == 0;
      sharc.clear |= frame.localScale[1] != sharc.localRange;
      std::copy_n(frame.localConfig, 4, sharc.localConfig);
      sharc.localIntensity = frame.localScale[0];
      sharc.localRange = frame.localScale[1];
    }
    const bool dynamicChanged = sharc.dynamicGeometry != dynamicGeometry;
    if (localChanged || difference > 1e-7f || angle < .9999999f || dynamicChanged)
      sharc.responsiveFrames = 32;
    else if (sharc.responsiveFrames)
      --sharc.responsiveFrames;
    if (dynamicChanged)
      sharc.dynamicGeometry = dynamicGeometry;
    std::copy_n(light, 9, sharc.light);
    if (sharc.clear)
      sharc.warmFrames = 0;
    frame.sharcHash = sharc.hash->address;
    frame.sharcAccumulation = sharc.accumulation->address;
    frame.sharcResolved = sharc.resolved->address;
    frame.sharcCounters = sharc.counters->address;
    frame.sharcConfig[0] = hasReceivers;
    frame.sharcConfig[1] = sharc.stats.capacity;
    frame.sharcConfig[2] = options.sharcDownscale;
    frame.sharcConfig[3] = options.sharcDebug;
    frame.sharcGrid[0] = float(options.sharcSceneScale);
    frame.sharcGrid[1] = 0;
    frame.sharcGrid[2] = float(sharc.responsiveFrames ? std::min(options.sharcAccumulationFrames, 4u)
                                                    : options.sharcAccumulationFrames);
    frame.sharcGrid[3] = float(options.sharcStaleFrames);
    std::copy_n(sharc.clear ? frame.eye : sharc.previousEye, 3, frame.sharcPreviousCamera);
    std::copy_n(frame.eye, 3, sharc.previousEye);
  }

  void PathTracer::Gpu::recordSharc(VkCommandBuffer command, const TraceOptions &options) {
    if (!sharc.stats.active || !sharc.hasReceivers)
      return;
    // Previous resolve/query (including confidence) must finish before either
    // transfer clears or this frame's atomic updates can reuse the buffers.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (sharc.clear) {
      for (const auto *buffer : {sharc.hash.get(), sharc.accumulation.get(), sharc.resolved.get()})
        vkCmdFillBuffer(command, buffer->handle, 0, VK_WHOLE_SIZE, 0);
      sharc.clear = false;
      ++sharc.stats.resets;
    }
    vkCmdFillBuffer(command, sharc.counters->handle, 0, VK_WHOLE_SIZE, 0);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    // WAW/RAW dependency also covers frames that did not clear the cache.
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (sharc.timestamps) {
      vkCmdResetQueryPool(command, sharc.timestamps, 0, 4);
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, sharc.timestamps, 0);
    }
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, selectLightingPipeline(sharc.update));
    const uint32_t tile = options.sharcDownscale * 8;
    {
      const uint32_t scale = std::max(options.sharcDownscale, 1u);
      gpu::GpuScope scope(command, "SHARC update (trace)", (extent.width + scale - 1) / scale,
                          (extent.height + scale - 1) / scale);
      vkCmdDispatch(command, (extent.width + tile - 1) / tile, (extent.height + tile - 1) / tile, 1);
    }
    if (sharc.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, sharc.timestamps, 1);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (sharc.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, sharc.timestamps, 2);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, sharc.resolve);
    {
      gpu::GpuScope scope(command, "SHARC resolve (entries)", uint32_t(sharc.stats.capacity), 1);
      vkCmdDispatch(command, (sharc.stats.capacity + 127) / 128, 1, 1);
    }
    if (sharc.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, sharc.timestamps, 3);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }

  void PathTracer::Gpu::finishSharc(VkCommandBuffer command) {
    if (!sharc.stats.active || !sharc.hasReceivers)
      return;
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy copy{.size = 16};
    vkCmdCopyBuffer(command, sharc.counters->handle, sharc.readback->handle, 1, &copy);
    barrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_HOST_BIT, VK_ACCESS_HOST_READ_BIT);
    sharc.pending = true;
  }

  remixapi_SharcStatisticsEXT PathTracer::sharcStatistics() const { return m_gpu->sharc.stats; }
} // namespace remix
