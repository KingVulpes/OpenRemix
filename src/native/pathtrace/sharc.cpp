#include "pathtrace/gpu.h"
#include "pathtrace/shaders/embedded.h"

namespace remix {
  using namespace trace::detail;

  void PathTracer::Gpu::sharcPipeline(const uint32_t *code, size_t size, VkPipelineLayout pipelineLayout,
                                     VkPipeline &out) {
    if (out)
      return;
    VkShaderModuleCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
    VkShaderModule shader{};
    check(vkCreateShaderModule(device, &ci, nullptr, &shader));
    VkComputePipelineCreateInfo pi{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = shader, .pName = "main"},
        .layout = pipelineLayout};
    const auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &out);
    vkDestroyShaderModule(device, shader, nullptr);
    check(result);
    lightingPipelines[out] = {code, size, pipelineLayout};
  }

  void PathTracer::Gpu::destroySharc(bool temporalOnly) {
    for (auto *pipeline : {&sharc.temporal, &sharc.temporalTransparency, &sharc.confidence}) {
      forgetLightingPipeline(*pipeline);
      if (*pipeline)
        vkDestroyPipeline(device, *pipeline, nullptr);
      *pipeline = VK_NULL_HANDLE;
    }
    if (temporalOnly)
      return;
    for (auto *pipeline : {&sharc.update, &sharc.resolve, &sharc.query, &sharc.transparency}) {
      forgetLightingPipeline(*pipeline);
      if (*pipeline)
        vkDestroyPipeline(device, *pipeline, nullptr);
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
      // beginFrame runs only after the previous submission fence. Host memory
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
        destroySharc();
        constexpr auto usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        sharc.hash = buffer(uint64_t(capacity) * 8, usage);
        sharc.accumulation = buffer(uint64_t(capacity) * 16, usage);
        sharc.resolved = buffer(uint64_t(capacity) * 16, usage);
        sharc.counters = buffer(16, usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        sharc.readback = buffer(16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
        s.capacity = capacity;
        s.memoryBytes = sharc.hash->size + sharc.accumulation->size + sharc.resolved->size +
                        sharc.counters->size + sharc.readback->size;
      }
      sharcPipeline(trace::sharcUpdateShader, sizeof(trace::sharcUpdateShader), layout, sharc.update);
      sharcPipeline(trace::sharcResolveShader, sizeof(trace::sharcResolveShader), layout, sharc.resolve);
      sharcPipeline(trace::sharcQueryShader, sizeof(trace::sharcQueryShader), layout, sharc.query);
      sharcPipeline(trace::sharcTransparencyShader, sizeof(trace::sharcTransparencyShader), layout, sharc.transparency);
      if (denoiser) {
        if (postLocal && postSigma)
          sharcPipeline(trace::sharcLocalSigmaTemporalShader, sizeof(trace::sharcLocalSigmaTemporalShader), temporalLayout, sharc.temporal);
        else if (postLocal)
          sharcPipeline(trace::sharcLocalTemporalShader, sizeof(trace::sharcLocalTemporalShader), temporalLayout, sharc.temporal);
        else if (postSigma)
          sharcPipeline(trace::sharcSigmaTemporalShader, sizeof(trace::sharcSigmaTemporalShader), temporalLayout, sharc.temporal);
        else
          sharcPipeline(trace::sharcTemporalShader, sizeof(trace::sharcTemporalShader), temporalLayout, sharc.temporal);
        sharcPipeline(trace::sharcTemporalTransparencyShader, sizeof(trace::sharcTemporalTransparencyShader), temporalLayout,
                      sharc.temporalTransparency);
        if (postReblur)
          sharcPipeline(trace::sharcConfidenceShader, sizeof(trace::sharcConfidenceShader), temporalLayout, sharc.confidence);
      }
      if (!sharc.timestamps && timestamps) {
        VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 4};
        check(vkCreateQueryPool(device, &qi, nullptr, &sharc.timestamps));
      }
      s.active = 1;
    } catch (VkResult r) {
      if (r == VK_ERROR_DEVICE_LOST)
        throw;
      destroySharc();
      const auto reason = "SHARC initialization failed (Vulkan " + std::to_string(r) + "); uncached tracing is active";
      std::strncpy(s.unavailableReason, reason.c_str(), sizeof(s.unavailableReason) - 1);
    } catch (const std::exception &e) {
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
                                      std::vector<uint8_t> geometry, std::vector<uint8_t> dynamicGeometry) {
    if (!sharc.stats.active)
      return;
    sharc.clear |= sharc.epoch != currentEpoch || sharc.reset != options.sharcReset ||
                   sharc.scale != options.sharcSceneScale || sharc.bounces != options.bounces ||
                   sharc.geometry != geometry;
    sharc.epoch = currentEpoch;
    sharc.reset = options.sharcReset;
    sharc.scale = options.sharcSceneScale;
    sharc.bounces = options.bounces;
    sharc.geometry = std::move(geometry);
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
                              frame.localScale[0] != sharc.localIntensity;
    if (localChanged) {
      sharc.warmFrames = 0;
      // Complete darkness must not reuse cached torch radiance.
      sharc.clear |= sharc.localConfig[0] != 0 && frame.localConfig[0] == 0;
      std::copy_n(frame.localConfig, 4, sharc.localConfig);
      sharc.localIntensity = frame.localScale[0];
    }
    if (localChanged || difference > 1e-7f || angle < .9999999f || sharc.dynamicGeometry != dynamicGeometry)
      sharc.responsiveFrames = 32;
    else if (sharc.responsiveFrames)
      --sharc.responsiveFrames;
    sharc.dynamicGeometry = std::move(dynamicGeometry);
    std::copy_n(light, 9, sharc.light);
    if (sharc.clear)
      sharc.warmFrames = 0;
    frame.sharcHash = sharc.hash->address;
    frame.sharcAccumulation = sharc.accumulation->address;
    frame.sharcResolved = sharc.resolved->address;
    frame.sharcCounters = sharc.counters->address;
    frame.sharcConfig[0] = 1;
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
    if (!sharc.stats.active)
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
    vkCmdDispatch(command, (extent.width + tile - 1) / tile, (extent.height + tile - 1) / tile, 1);
    if (sharc.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, sharc.timestamps, 1);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    if (sharc.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, sharc.timestamps, 2);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, sharc.resolve);
    vkCmdDispatch(command, (sharc.stats.capacity + 127) / 128, 1, 1);
    if (sharc.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, sharc.timestamps, 3);
    barrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_WRITE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }

  void PathTracer::Gpu::finishSharc(VkCommandBuffer command) {
    if (!sharc.stats.active)
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
