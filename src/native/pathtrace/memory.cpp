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
    ScopedMilliseconds time{performance.allocationMilliseconds};
    auto out = std::make_unique<Buffer>();
    out->device = device;
    out->size = align(std::max(size, VkDeviceSize(16)), 4096);
    out->usage = usage | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    out->host = host;
    if (reusable) {
      if (auto cached = bufferCache->take(out->size, out->usage, host)) {
        ++performance.bufferCacheHits;
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
    VkMemoryDedicatedRequirements dedicated{.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS};
    VkMemoryRequirements2 query{.sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dedicated};
    VkBufferMemoryRequirementsInfo2 request{.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
                                           .buffer = out->handle};
    vkGetBufferMemoryRequirements2(device, &request, &query);
    const auto &requirements = query.memoryRequirements;
    out->allocationSize = requirements.size;
    ++performance.bufferAllocations;
    performance.allocationBytes += requirements.size;
    VkMemoryAllocateFlagsInfo flags = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                       .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT};
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &flags,
        .allocationSize = requirements.size,
        .memoryTypeIndex =
            memoryType(requirements.memoryTypeBits,
                       host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    if (reusable && !dedicated.requiresDedicatedAllocation) {
      const VkDeviceSize alignment = std::max(requirements.alignment,
          host ? properties.limits.nonCoherentAtomSize : VkDeviceSize(1));
      out->allocationSize = align(requirements.size,alignment);
      out->atomSize = host ? properties.limits.nonCoherentAtomSize : 1;
      for (const auto &block : bufferMemory) if (block->type == allocate.memoryTypeIndex) {
        if (const auto offset = block->take(out->allocationSize,alignment)) {
          out->block = block; out->memoryOffset = *offset; break;
        }
      }
      if (!out->block) {
        auto block = std::make_shared<BufferMemory>();
        block->device = device; block->type = allocate.memoryTypeIndex;
        block->size = std::max(VkDeviceSize(16*1024*1024),out->allocationSize);
        allocate.allocationSize = block->size;
        check(vkAllocateMemory(device,&allocate,nullptr,&block->memory));
        ++performance.memoryAllocations; performance.memoryAllocationBytes += block->size;
        block->free.emplace_back(0,block->size);
        const auto offset = block->take(out->allocationSize,alignment);
        out->block = block; out->memoryOffset = *offset;
        bufferMemory.push_back(std::move(block));
      }
      out->memory = out->block->memory;
      if (host) {
        if (!out->block->mapped)
          check(vkMapMemory(device,out->memory,0,VK_WHOLE_SIZE,0,&out->block->mapped));
        out->mapped = static_cast<char*>(out->block->mapped)+out->memoryOffset;
      }
    } else {
      VkMemoryDedicatedAllocateInfo dedicatedInfo{.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
                                                   .buffer = out->handle};
      if (dedicated.requiresDedicatedAllocation) flags.pNext = &dedicatedInfo;
      check(vkAllocateMemory(device, &allocate, nullptr, &out->memory));
      ++performance.memoryAllocations; performance.memoryAllocationBytes += allocate.allocationSize;
    }
    check(vkBindBufferMemory(device, out->handle, out->memory, out->memoryOffset));
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
    ScopedMilliseconds time{performance.uploadMilliseconds};
    performance.uploadBytes += size;
    // Slot ownership already protects host writes. Keep reusable staging mapped
    // through its lifetime, including time in the bounded idle allocation cache.
    if (!buffer.mapped)
      check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
    std::memcpy(buffer.mapped, data, size);
    VkMappedMemoryRange flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = buffer.memory,
        .offset = buffer.memoryOffset,
        .size = buffer.block ? align(size,buffer.atomSize) : VK_WHOLE_SIZE};
    const auto result = vkFlushMappedMemoryRanges(device, 1, &flush);
    check(result);
  }

  void PathTracer::Gpu::write(Buffer &buffer, std::initializer_list<std::pair<const void *, size_t>> parts,
                              size_t offset) {
    size_t size = 0;
    for (const auto &part : parts)
      size += part.second;
    if (size <= offset)
      return;
    ScopedMilliseconds time{performance.uploadMilliseconds};
    performance.uploadBytes += size - offset;
    if (!buffer.mapped)
      check(vkMapMemory(device, buffer.memory, 0, VK_WHOLE_SIZE, 0, &buffer.mapped));
    size_t position = 0;
    for (const auto &[data, bytes] : parts) {
      const size_t skip = offset > position ? std::min(offset - position, bytes) : 0;
      if (bytes > skip)
        std::memcpy(static_cast<char *>(buffer.mapped) + position + skip, static_cast<const char *>(data) + skip,
                    bytes - skip);
      position += bytes;
    }
    VkMappedMemoryRange flush = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, .memory = buffer.memory,
        .offset = buffer.memoryOffset,
        .size = buffer.block ? align(size, buffer.atomSize) : VK_WHOLE_SIZE};
    check(vkFlushMappedMemoryRanges(device, 1, &flush));
  }

  void PathTracer::Gpu::hostBuffer(std::unique_ptr<Buffer> &out, const void *data, size_t size,
                                   VkBufferUsageFlags usage) {
    if (!out || out->size < size)
      out = buffer(size, usage, true);
    write(*out, data, size);
  }

  VkDeviceSize PathTracer::Gpu::stage(const void *data, size_t size) {
    VkDeviceSize offset = align(staging.used, 16);
    if (!staging.buffer || offset + size > staging.buffer->size) {
      // Copies recorded from the full buffer keep it until this slot's fence.
      const VkDeviceSize capacity =
          std::max<VkDeviceSize>({VkDeviceSize(size), staging.buffer ? staging.buffer->size * 2 : 0,
                                  VkDeviceSize(4 * 1024 * 1024)});
      if (staging.buffer)
        uploads.push_back(std::move(staging.buffer));
      staging.buffer = buffer(capacity, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
      offset = 0;
    }
    ScopedMilliseconds time{performance.uploadMilliseconds};
    performance.uploadBytes += size;
    auto &target = *staging.buffer;
    if (!target.mapped)
      check(vkMapMemory(device, target.memory, 0, VK_WHOLE_SIZE, 0, &target.mapped));
    std::memcpy(static_cast<char *>(target.mapped) + offset, data, size);
    const VkDeviceSize atom = std::max<VkDeviceSize>(1, properties.limits.nonCoherentAtomSize);
    const VkDeviceSize begin = offset / atom * atom, end = align(offset + size, atom);
    VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                 .memory = target.memory,
                                 .offset = begin,
                                 .size = end > target.allocationSize ? VK_WHOLE_SIZE : end - begin};
    check(vkFlushMappedMemoryRanges(device, 1, &flush));
    staging.used = offset + size;
    return offset;
  }

  std::unique_ptr<Buffer> PathTracer::Gpu::upload(VkCommandBuffer command, const void *data, size_t size,
                                                  VkBufferUsageFlags usage) {
    const VkDeviceSize offset = stage(data, size);
    auto out = buffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false, true);
    VkBufferCopy copy = {.srcOffset = offset, .size = size};
    vkCmdCopyBuffer(command, staging.buffer->handle, out->handle, 1, &copy);
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
