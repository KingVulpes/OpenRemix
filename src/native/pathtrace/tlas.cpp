#include "pathtrace/gpu.h"

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  void PathTracer::Gpu::buildTop(VkCommandBuffer command, const std::vector<trace::InstanceInput> inputs[2],
                                 bool force) {
    ScopedMilliseconds time{performance.accelerationMilliseconds};
    VkAccelerationStructureGeometryKHR geometry[2] = {};
    VkAccelerationStructureBuildGeometryInfoKHR builds[2] = {};
    VkAccelerationStructureBuildSizesInfoKHR sizes[2] = {};
    uint32_t reasons[2] = {}, count[2] = {};
    bool changed[2] = {};
    VkDeviceSize scratchBytes = 0;
    for (unsigned i = 0; i < 2; ++i) {
      auto &top = tlas[i];
      reasons[i] = top.policy.reason(inputs[i], i != 0, force && i != 0);
      changed[i] = reasons[i] || top.policy.changed(inputs[i]);
      if (i == 1)
        stats.dynamicRebuildReason = changed[i] ? reasons[i] : 0;
      if (!changed[i])
        continue;
      count[i] = uint32_t(inputs[i].size());
      if (count[i] > asProperties.maxInstanceCount)
        throw VK_ERROR_TOO_MANY_OBJECTS;
      std::vector<VkAccelerationStructureInstanceKHR> data;
      for (const auto &input : inputs[i]) {
        VkAccelerationStructureInstanceKHR instance = {};
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 4; ++c)
            instance.transform.matrix[r][c] = input.model[c * 4 + r];
        instance.instanceCustomIndex = input.customIndex;
        instance.mask = input.mask;
        const auto &m = input.model;
        const float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                          m[8] * (m[1] * m[6] - m[5] * m[2]);
        // Culled instances match the shaders' one-sided front-face rejection,
        // letting hardware discard back faces before candidate traversal.
        instance.flags = (input.facingCulled ? 0u : VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR) |
                         (det > 0 ? VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR : 0u);
        instance.accelerationStructureReference = input.geometry;
        data.push_back(instance);
      }
      hostBuffer(top.instances, data.data(), data.size() * sizeof(data[0]), inputUsage);
      geometry[i] = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                     .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR};
      geometry[i].geometry.instances = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
          .data = {.deviceAddress = top.instances->address}};
      bool update = i == 1 && !reasons[i];
      builds[i] = {.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
                   .type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR,
                   .flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                            (i ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0u),
                   .mode = update ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR
                                  : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
                   .geometryCount = 1,
                   .pGeometries = &geometry[i]};
      sizes[i].sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR;
      buildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &builds[i], &count[i], &sizes[i]);
      if (!top.accel || top.accel->size < sizes[i].accelerationStructureSize) {
        if (top.accel)
          retired.push_back(std::move(top.accel));
        top.accel =
            acceleration(sizes[i].accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR);
      }
      builds[i].srcAccelerationStructure = update ? top.accel->handle : VK_NULL_HANDLE;
      builds[i].dstAccelerationStructure = top.accel->handle;
      scratchBytes += align(update ? sizes[i].updateScratchSize : sizes[i].buildScratchSize,
                            asProperties.minAccelerationStructureScratchOffsetAlignment);
    }
    if (!changed[0] && !changed[1])
      return;
    if (!topScratch ||
        topScratch->size < scratchBytes + asProperties.minAccelerationStructureScratchOffsetAlignment)
      topScratch = buffer(scratchBytes + asProperties.minAccelerationStructureScratchOffsetAlignment,
                          VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    VkDeviceAddress address =
        align(topScratch->address, asProperties.minAccelerationStructureScratchOffsetAlignment);
    for (unsigned i = 0; i < 2; ++i) {
      if (!changed[i])
        continue;
      bool update = builds[i].mode == VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR;
      builds[i].scratchData.deviceAddress = address;
      VkAccelerationStructureBuildRangeInfoKHR range = {.primitiveCount = count[i]};
      const auto *ranges = &range;
      buildAs(command, 1, &builds[i], &ranges);
      address += align(update ? sizes[i].updateScratchSize : sizes[i].buildScratchSize,
                       asProperties.minAccelerationStructureScratchOffsetAlignment);
      tlas[i].policy.commit(inputs[i], reasons[i]);
      if (!i)
        ++stats.staticTlasBuilds;
      else if (update)
        ++stats.dynamicTlasRefits;
      else
        ++stats.dynamicTlasBuilds;
    }
    stats.dynamicRefitCount = tlas[1].policy.refits;
  }

} // namespace remix
