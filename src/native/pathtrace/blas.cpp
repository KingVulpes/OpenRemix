#include "pathtrace/gpu.h"

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  void PathTracer::Gpu::complete(VkCommandBuffer command) {
    completeLightGuiding();
    uploads.clear();
    retiredGeometry.clear();
    retiredBuffers.clear();
    retired.clear();
    if (timingPending) {
      uint64_t times[4] = {};
      if (vkGetQueryPoolResults(device, timestamps, 0, 4, sizeof(times), times, sizeof(uint64_t),
                                VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const uint64_t mask = timestampBits >= 64 ? UINT64_MAX : (uint64_t(1) << timestampBits) - 1;
        stats.gpuBuildMilliseconds =
            float((times[1] - times[0]) & mask) * properties.limits.timestampPeriod / 1e6f;
        stats.gpuTraceMilliseconds =
            float((times[3] - times[2]) & mask) * properties.limits.timestampPeriod / 1e6f;
        stats.gpuTimingsAvailable = 1;
      }
      timingPending = false;
    }
    if (!compactPending.empty()) {
      std::vector<uint64_t> sizes(compactPending.size());
      check(vkGetQueryPoolResults(device, compaction, 0, uint32_t(sizes.size()), sizes.size() * 8,
                                  sizes.data(), 8, VK_QUERY_RESULT_64_BIT));
      for (size_t i = 0; i < sizes.size(); ++i) {
        auto &geometry = *compactPending[i];
        if (!sizes[i] || sizes[i] >= geometry.accel->size)
          continue;
        auto compact = acceleration(sizes[i], VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
        VkCopyAccelerationStructureInfoKHR copy = {.sType =
                                                       VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR,
                                                   .src = geometry.accel->handle,
                                                   .dst = compact->handle,
                                                   .mode = VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR};
        copyAs(command, &copy);
        // The mesh may disappear in this frame (including a callback's empty
        // redraw). Both copy endpoints remain referenced by this command buffer.
        retired.push_back(compact);
        retired.push_back(std::move(geometry.accel));
        geometry.accel = std::move(compact);
        ++stats.blasCompactions;
      }
      compactPending.clear();
    }
    for (auto it = geometryCache.begin(); it != geometryCache.end();) {
      if (it->second.expired())
        it = geometryCache.erase(it);
      else
        ++it;
    }
    // Release empty arena blocks after the last-use fence; retain one for reuse.
    for (auto it = arenas.begin(); it != arenas.end() && arenas.size() > 1;) {
      if (it->use_count() == 1)
        it = arenas.erase(it);
      else
        ++it;
    }
  }

  void PathTracer::Gpu::buildBottom(VkCommandBuffer command) {
    std::vector<std::shared_ptr<Geometry>> work;
    VkDeviceSize scratchBytes = 0;
    for (auto &[key, view] : meshes)
      if (view.geometry->pending) {
        // Geometry can be shared by multiple material variants.
        if (std::find(work.begin(), work.end(), view.geometry) == work.end())
          work.push_back(view.geometry);
      }
    if (work.empty())
      return;
    for (const auto &geometry : work)
      scratchBytes +=
          align(geometry->update ? geometry->sizes.updateScratchSize : geometry->sizes.buildScratchSize,
                asProperties.minAccelerationStructureScratchOffsetAlignment);
    if (!scratch ||
        scratch->size < scratchBytes + asProperties.minAccelerationStructureScratchOffsetAlignment)
      scratch = buffer(scratchBytes + asProperties.minAccelerationStructureScratchOffsetAlignment,
                       VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDeviceAddress address =
        align(scratch->address, asProperties.minAccelerationStructureScratchOffsetAlignment);
    std::vector<VkAccelerationStructureBuildGeometryInfoKHR> builds;
    std::vector<const VkAccelerationStructureBuildRangeInfoKHR *> ranges;
    for (auto &g : work) {
      builds.push_back({.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                        .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
                        .flags = flags(g->deform),
                        .mode = g->update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                          : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                        .srcAccelerationStructure = g->update ? g->accel->handle : VK_NULL_HANDLE,
                        .dstAccelerationStructure = g->accel->handle,
                        .geometryCount = uint32_t(g->geometries.size()),
                        .pGeometries = g->geometries.data(),
                        .scratchData = {.deviceAddress = address}});
      ranges.push_back(g->ranges.data());
      address += align(g->update ? g->sizes.updateScratchSize : g->sizes.buildScratchSize,
                       asProperties.minAccelerationStructureScratchOffsetAlignment);
      if (g->update)
        ++stats.blasRefits;
      else
        ++stats.blasBuilds;
      if (!g->deform)
        compactPending.push_back(g);
      g->pending = false;
    }
    buildAs(command, uint32_t(builds.size()), builds.data(), ranges.data());
    barrier(command, buildStage, asWrite, buildStage, asRead);
    if (!compactPending.empty()) {
      if (compaction)
        vkDestroyQueryPool(device, compaction, nullptr);
      VkQueryPoolCreateInfo info = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                    .queryType = VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR,
                                    .queryCount = uint32_t(compactPending.size())};
      check(vkCreateQueryPool(device, &info, nullptr, &compaction));
      vkCmdResetQueryPool(command, compaction, 0, uint32_t(compactPending.size()));
      std::vector<VkAccelerationStructureKHR> handles;
      for (const auto &g : compactPending)
        handles.push_back(g->accel->handle);
      writeAsProperties(command, uint32_t(handles.size()), handles.data(),
                        VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, compaction, 0);
    }
  }

} // namespace remix
