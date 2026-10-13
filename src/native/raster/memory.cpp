#include "raster/gpu.h"

namespace remix {
  using namespace scene;
  using namespace raster::detail;

  void RasterRenderer::Gpu::recycle(std::unique_ptr<Buffer> buffer) {
    // Called only after the frame fence completes. The buffer just used is the
    // one most likely to be asked for again (a frame's staging, skinned
    // vertices and transient meshes come back every frame), so it is always
    // kept and what the pool has held longest goes when it is over its bound.
    // A full pool that turned the newcomer away instead kept leftovers of one
    // large upload forever and freed and allocated device memory every frame,
    // which costs milliseconds a buffer (Garry's Mod, raster backend: 7-25 ms
    // of a frame's preparation). One eviction a call: a pool far over its
    // bound empties over some frames, not in one.
    if (!buffer)
      return;
    constexpr uint64_t bound = 64u * 1024u * 1024u;
    reusableBytes += buffer->allocationSize;
    reusableBuffers.push_back(std::move(buffer));
    if (reusableBytes > bound && reusableBuffers.size() > 1) {
      reusableBytes -= reusableBuffers.front()->allocationSize;
      reusableBuffers.erase(reusableBuffers.begin());
    }
  }

  uint32_t RasterRenderer::Gpu::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags) {
        return i;
      }
    }
    throw VK_ERROR_FEATURE_NOT_PRESENT;
  }

  std::unique_ptr<Buffer> RasterRenderer::Gpu::allocateBuffer(size_t size, VkBufferUsageFlags usage,
                                                              VkMemoryPropertyFlags flags) {
    if (!size) {
      throw VK_ERROR_INITIALIZATION_FAILED;
    }
    auto best = reusableBuffers.end();
    for (auto it = reusableBuffers.begin(); it != reusableBuffers.end(); ++it) {
      if ((*it)->usage == usage && (*it)->flags == flags && (*it)->capacity >= size &&
          (best == reusableBuffers.end() || (*it)->capacity < (*best)->capacity))
        best = it;
    }
    if (best != reusableBuffers.end()) {
      auto out = std::move(*best);
      reusableBytes -= out->allocationSize;
      reusableBuffers.erase(best);
      return out;
    }
    // Capacities in a few sizes, so a buffer fits the next request of about
    // its size (a frame's meshes and vertex counts vary): powers of two up to
    // 1 MiB, 64 KiB steps above (large buffers are mostly kept, not recycled).
    if (size <= 1024u * 1024u) {
      size_t capacity = 4096;
      while (capacity < size)
        capacity *= 2;
      size = capacity;
    } else
      size = (size + 65535) & ~size_t(65535);
    auto out = std::make_unique<Buffer>();
    out->device = device;
    out->capacity = size;
    out->usage = usage;
    out->flags = flags;
    const VkBufferCreateInfo info = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                     .size = size,
                                     .usage = usage,
                                     .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateBuffer(device, &info, nullptr, &out->buffer));
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, out->buffer, &requirements);
    out->allocationSize = requirements.size;
    const VkMemoryAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                             .allocationSize = requirements.size,
                                             .memoryTypeIndex =
                                                 memoryType(requirements.memoryTypeBits, flags)};
    if (flags == VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
      stagingMemoryFlags = memory.memoryTypes[allocation.memoryTypeIndex].propertyFlags;
    check(vkAllocateMemory(device, &allocation, nullptr, &out->memory));
    check(vkBindBufferMemory(device, out->buffer, out->memory, 0));
    return out;
  }

  std::unique_ptr<Buffer> RasterRenderer::Gpu::buffer(const void *data, size_t size,
                                                      VkBufferUsageFlags usage) {
    return buffer(size, usage, [&](uint8_t *mapped) { std::memcpy(mapped, data, size); });
  }

  std::unique_ptr<Buffer> RasterRenderer::Gpu::buffer(size_t size, VkBufferUsageFlags usage,
                                                      const std::function<void(uint8_t *)> &write) {
    const auto begin = std::chrono::steady_clock::now();
    const size_t pooled = reusableBuffers.size();
    auto out = allocateBuffer(size, usage, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    stagingAllocated += reusableBuffers.size() == pooled; // not taken from the pool
    ++stagingBuffers;
    uploadBytes += size;
    void *mapped;
    check(vkMapMemory(device, out->memory, 0, VK_WHOLE_SIZE, 0, &mapped));
    const auto mappedAt = std::chrono::steady_clock::now();
    stagingAllocateMs += std::chrono::duration<double, std::milli>(mappedAt - begin).count();
    write(static_cast<uint8_t *>(mapped));
    stagingWriteMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - mappedAt).count();
    const VkMappedMemoryRange flush = {.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                       .memory = out->memory,
                                       .offset = 0,
                                       .size = VK_WHOLE_SIZE};
    const auto result = vkFlushMappedMemoryRanges(device, 1, &flush);
    vkUnmapMemory(device, out->memory);
    check(result);
    return out;
  }

  void RasterRenderer::Gpu::recycle(std::unique_ptr<Image> image) {
    // Called after the image's last-use fence. Only large images are worth
    // keeping; a few suffice for a per-frame overlay in flight.
    if (!image || image->bytes < 1024 * 1024)
      return;
    reusableImages.push_back(std::move(image));
    if (reusableImages.size() > 4)
      reusableImages.erase(reusableImages.begin());
  }

  std::unique_ptr<Buffer> RasterRenderer::Gpu::geometryBuffer(const void *data, size_t size,
                                                              VkBufferUsageFlags usage,
                                                              VkCommandBuffer command) {
    // Geometry stays on the GPU, including devices without CPU-visible local memory.
    auto out =
        allocateBuffer(size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uploads.push_back(buffer(data, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    const VkBufferCopy copy = {.size = size};
    vkCmdCopyBuffer(command, uploads.back()->buffer, out->buffer, 1, &copy);
    VkAccessFlags reads = 0;
    if (usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) {
      reads |= VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT;
    }
    if (usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
      reads |= VK_ACCESS_INDEX_READ_BIT;
    }
    const VkBufferMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                           .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                           .dstAccessMask = reads,
                                           .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                           .buffer = out->buffer,
                                           .size = VK_WHOLE_SIZE};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, 0,
                         nullptr, 1, &barrier, 0, nullptr);
    // prepare() releases staging buffers only after the previous frame fence completes.
    return out;
  }

  std::unique_ptr<Image> RasterRenderer::Gpu::image(uint32_t width, uint32_t height, uint32_t levels,
                                                    VkFormat format, bool isDepth, VkImageCreateFlags flags,
                                                    VkComponentMapping components) {
    if (width > properties.limits.maxImageDimension2D || height > properties.limits.maxImageDimension2D) {
      throw VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    if (!isDepth)
      for (auto it = reusableImages.begin(); it != reusableImages.end(); ++it) {
        const auto &image = **it;
        if (image.width == width && image.height == height && image.levels == levels && image.format == format &&
            image.flags == flags && std::memcmp(&image.components, &components, sizeof(components)) == 0) {
          auto out = std::move(*it);
          reusableImages.erase(it);
          return out;
        }
      }
    auto out = std::make_unique<Image>();
    out->width = width;
    out->height = height;
    out->flags = flags;
    out->device = device;
    out->format = format;
    out->levels = levels;
    out->components = components;
    const VkImageCreateInfo info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = flags,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = levels,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        // A depth image is the target's attachment or the sampled copy sprite
        // overlays take of it (captureSceneDepth).
        .usage = isDepth ? VkImageUsageFlags(VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                                             VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                             VK_IMAGE_USAGE_SAMPLED_BIT)
                         : VkImageUsageFlags(VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT),
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    check(vkCreateImage(device, &info, nullptr, &out->image));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, out->image, &requirements);
    const VkMemoryAllocateInfo allocation = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(device, &allocation, nullptr, &out->memory));
    out->bytes = requirements.size;
    check(vkBindImageMemory(device, out->image, out->memory, 0));
    const VkImageViewCreateInfo view = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components = components,
        .subresourceRange = {VkImageAspectFlags(isDepth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT), 0, levels, 0,
                             1}};
    check(vkCreateImageView(device, &view, nullptr, &out->view));
    return out;
  }

} // namespace remix
