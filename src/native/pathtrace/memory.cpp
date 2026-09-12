#include "pathtrace/gpu.h"

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  uint32_t PathTracer::Gpu::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw VK_ERROR_FEATURE_NOT_PRESENT;
  }

  std::unique_ptr<Buffer> PathTracer::Gpu::buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host,
                                                 bool reusable) {
    auto out = std::make_unique<Buffer>();
    out->device = device;
    out->size = align(std::max(size, VkDeviceSize(16)), 4096);
    out->usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    out->host = host;
    if (reusable) {
      if (auto cached = bufferCache->take(out->size, out->usage, host)) {
        static_cast<BufferAllocation &>(*out) = *cached;
        out->cache = bufferCache;
        return out;
      }
    }
    VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = out->size,
                               .usage = out->usage,
                               .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateBuffer(device, &info, nullptr, &out->handle));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, out->handle, &requirements);
    out->allocationSize = requirements.size;
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex =
            memoryType(requirements.memoryTypeBits,
                       host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(device, &allocate, nullptr, &out->memory));
    check(vkBindBufferMemory(device, out->handle, out->memory, 0));
    VkBufferDeviceAddressInfo address = {.sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                                         .buffer = out->handle};
    out->address = getBufferAddress(device, &address);
    // Failed or partially initialized allocations always take the ordinary destruction path.
    if (reusable)
      out->cache = bufferCache;
    return out;
  }

  void PathTracer::Gpu::write(Buffer &buffer, const void *data, size_t size) {
    if (!size)
      return;
    void *mapped;
    check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    std::memcpy(mapped, data, size);
    VkMappedMemoryRange flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = buffer.memory, .size = VK_WHOLE_SIZE};
    const auto result = vkFlushMappedMemoryRanges(device, 1, &flush);
    vkUnmapMemory(device, buffer.memory);
    check(result);
  }

  void PathTracer::Gpu::hostBuffer(std::unique_ptr<Buffer> &out, const void *data, size_t size,
                                   VkBufferUsageFlags usage) {
    if (!out || out->size < size)
      out = buffer(size, usage, true);
    write(*out, data, size);
  }

  std::unique_ptr<Buffer> PathTracer::Gpu::upload(VkCommandBuffer command, const void *data, size_t size,
                                                  VkBufferUsageFlags usage) {
    auto staging = buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, true);
    write(*staging, data, size);
    auto out = buffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true);
    VkBufferCopy copy = {.size = size};
    vkCmdCopyBuffer(command, staging->handle, out->handle, 1, &copy);
    uploads.push_back(std::move(staging));
    return out;
  }

  std::shared_ptr<Accel> PathTracer::Gpu::acceleration(VkDeviceSize size,
                                                       VkAccelerationStructureTypeKHR type) {
    size = align(size, 256);
    std::shared_ptr<Arena> arena;
    VkDeviceSize offset = 0;
    for (auto &candidate : arenas) {
      auto range = std::find_if(candidate->free.begin(), candidate->free.end(),
                                [&](auto value) { return value.second >= size; });
      if (range == candidate->free.end())
        continue;
      arena = candidate;
      offset = range->first;
      auto remaining = range->second - size;
      candidate->free.erase(range);
      if (remaining)
        candidate->free.emplace(offset + size, remaining);
      break;
    }
    if (!arena) {
      arena = std::make_shared<Arena>();
      arena->buffer = buffer(std::max(size, VkDeviceSize(16 * 1024 * 1024)),
                             VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR);
      if (arena->buffer->size > size)
        arena->free.emplace(size, arena->buffer->size - size);
      arenas.push_back(arena);
    }
    auto out = std::make_shared<Accel>();
    out->device = device;
    out->destroy = destroyAs;
    out->arena = arena;
    out->offset = offset;
    out->size = size;
    VkAccelerationStructureCreateInfoKHR info = {.sType =
                                                     VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
                                                 .buffer = arena->buffer->handle,
                                                 .offset = offset,
                                                 .size = size,
                                                 .type = type};
    check(createAs(device, &info, nullptr, &out->handle));
    VkAccelerationStructureDeviceAddressInfoKHR address = {
        .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
        .accelerationStructure = out->handle};
    out->address = getAsAddress(device, &address);
    return out;
  }

} // namespace remix
