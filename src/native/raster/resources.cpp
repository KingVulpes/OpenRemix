#include "raster/gpu.h"
#include "runtime/pipelines.h"
#include "scene/images.h"
#include <remix/remix_native_ext.h>

namespace remix {
  using namespace scene;
  using namespace raster::detail;

  namespace {
    VkFormat blockFormat(uint32_t format) {
      switch (format) {
      case REMIXAPI_FORMAT_BC1_RGB_SRGB:
        return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
      case REMIXAPI_FORMAT_BC1_RGB_UNORM:
        return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT:
        return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
      case REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC3_SRGB:
        return VK_FORMAT_BC3_SRGB_BLOCK;
      case REMIXAPI_FORMAT_BC3_UNORM:
        return VK_FORMAT_BC3_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC4_UNORM_EXT:
        return VK_FORMAT_BC4_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC5_UNORM:
        return VK_FORMAT_BC5_UNORM_BLOCK;
      case REMIXAPI_FORMAT_BC6H_UFLOAT_EXT:
        return VK_FORMAT_BC6H_UFLOAT_BLOCK;
      case REMIXAPI_FORMAT_BC6H_SFLOAT_EXT:
        return VK_FORMAT_BC6H_SFLOAT_BLOCK;
      case REMIXAPI_FORMAT_BC7_SRGB:
        return VK_FORMAT_BC7_SRGB_BLOCK;
      case REMIXAPI_FORMAT_BC7_UNORM:
        return VK_FORMAT_BC7_UNORM_BLOCK;
      default:
        return VK_FORMAT_UNDEFINED;
      }
    }
    // Same-class UNORM counterpart used by linear data-map views.
    VkFormat linearFormat(VkFormat format) {
      switch (format) {
      case VK_FORMAT_R8G8B8A8_SRGB:
        return VK_FORMAT_R8G8B8A8_UNORM;
      case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
      case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
      case VK_FORMAT_BC3_SRGB_BLOCK:
        return VK_FORMAT_BC3_UNORM_BLOCK;
      case VK_FORMAT_BC7_SRGB_BLOCK:
        return VK_FORMAT_BC7_UNORM_BLOCK;
      default:
        return format;
      }
    }
  } // namespace

  RasterRenderer::Gpu::MeshGpu &RasterRenderer::Gpu::mesh(const std::shared_ptr<Surface> &source,
                                                          VkCommandBuffer command) {
    auto it = meshes.find(source.get());
    if (it != meshes.end()) {
      return it->second;
    }
    const size_t offset = source->vertices.size() * sizeof(Vertex);
    std::vector<uint8_t> data(offset + source->indices.size() * sizeof(uint32_t));
    std::memcpy(data.data(), source->vertices.data(), offset);
    std::memcpy(data.data() + offset, source->indices.data(), source->indices.size() * sizeof(uint32_t));
    MeshGpu value{source,
                  geometryBuffer(data.data(), data.size(),
                                 VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                                 command),
                  offset};
    return meshes.emplace(source.get(), std::move(value)).first->second;
  }

  RasterRenderer::Gpu::TextureGpu &RasterRenderer::Gpu::texture(const std::shared_ptr<Texture> &source,
                                                                VkCommandBuffer command) {
    auto it = textures.find(source.get());
    if (it != textures.end()) {
      return it->second;
    }
    if (source->pixels.compressed)
      return compressedTexture(source, command);
    return textures.emplace(source.get(), uploadPixels(source, source->pixels, command, 0)).first->second;
  }

  RasterRenderer::Gpu::TextureGpu RasterRenderer::Gpu::uploadPixels(const std::shared_ptr<Texture> &source,
                                                                    const Pixels &p, VkCommandBuffer command,
                                                                    VkImageCreateFlags flags) {
    const uint32_t levels = uint32_t(p.mips.size());
    TextureGpu value{source, image(p.width, p.height, levels,
                                   !p.hdrMips.empty() ? VK_FORMAT_R32G32B32A32_SFLOAT
                                   : p.srgb           ? VK_FORMAT_R8G8B8A8_SRGB
                                                      : VK_FORMAT_R8G8B8A8_UNORM,
                                   false, flags)};
    value.view = value.data->view;
    // Level bytes go straight into the staging buffer.
    auto level = [&](uint32_t i) -> std::pair<const uint8_t *, size_t> {
      if (p.hdrMips.empty())
        return {p.mips[i].data(), p.mips[i].size()};
      return {reinterpret_cast<const uint8_t *>(p.hdrMips[i].data()), p.hdrMips[i].size() * sizeof(float)};
    };
    std::vector<VkBufferImageCopy> copies;
    size_t total = 0;
    uint32_t width = p.width, height = p.height;
    for (uint32_t i = 0; i < levels; ++i) {
      copies.push_back({.bufferOffset = total,
                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1},
                        .imageExtent = {width, height, 1}});
      total += level(i).second;
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }
    uploads.push_back(buffer(total, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, [&](uint8_t *mapped) {
      for (uint32_t i = 0; i < levels; ++i) {
        const auto [bytes, size] = level(i);
        if (size)
          std::memcpy(mapped + copies[i].bufferOffset, bytes, size);
      }
    }));
    copyLevels(command, value.data->image, levels, copies);
    return value;
  }

  void RasterRenderer::Gpu::copyLevels(VkCommandBuffer command, VkImage image, uint32_t levels,
                                       const std::vector<VkBufferImageCopy> &copies) {
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(command, uploads.back()->buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels,
                           copies.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
  }

  RasterRenderer::Gpu::TextureGpu &RasterRenderer::Gpu::compressedTexture(const std::shared_ptr<Texture> &source,
                                                                          VkCommandBuffer command) {
    const auto &p = source->pixels;
    auto &c = *p.compressed;
    // A linear data map of an sRGB compressed texture (dataTexturePath) shares
    // the source's mutable-format image through a UNORM view: no second copy.
    if (const auto &base = source->encodedSource; base && base->pixels.compressed == p.compressed) {
      auto &image = *texture(base, command).data;
      if (linearFormat(image.format) == image.format)
        return textures.emplace(source.get(), TextureGpu{source, nullptr, image.view}).first->second;
      if (!image.linearView) {
        const VkImageViewCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                            .image = image.image,
                                            .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                            .format = linearFormat(image.format),
                                            .components = image.components,
                                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, image.levels, 0, 1}};
        check(vkCreateImageView(device, &info, nullptr, &image.linearView));
      }
      TextureGpu value{source, nullptr, image.linearView};
      return textures.emplace(source.get(), std::move(value)).first->second;
    }
    // sRGB images may later gain a linear alias view.
    const VkImageCreateFlags flags = p.srgb ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    auto release = [&] { std::vector<uint8_t>().swap(c.upload); };
    const VkFormat format = textureCompression ? blockFormat(c.format) : VK_FORMAT_UNDEFINED;
    if (c.upload.empty() || format == VK_FORMAT_UNDEFINED) {
      // Without BC sampling, decode once from the payload. A released payload
      // (never expected: images live as long as their texture) degrades to the
      // small preview level instead of failing the frame.
      Pixels fallback;
      if (!c.upload.empty()) {
        fallback = decodeCompressed(p);
      } else {
        fallback = {c.previewWidth, c.previewHeight, p.srgb, {c.preview}};
        if (!c.hdrPreview.empty())
          fallback.hdrMips = {c.hdrPreview};
      }
      auto value = uploadPixels(source, fallback, command, flags);
      release();
      return textures.emplace(source.get(), std::move(value)).first->second;
    }
    // BC4 is sampled as (r, 0, 0, 1); the decoded path replicates r into rgb.
    const VkComponentMapping components =
        c.block == 4 ? VkComponentMapping{VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                                          VK_COMPONENT_SWIZZLE_ONE}
                     : VkComponentMapping{};
    TextureGpu value{source, image(p.width, p.height, c.levels, format, false, flags, components)};
    value.view = value.data->view;
    const VkDeviceSize blockSize = (c.block == 1 || c.block == 4) ? 8 : 16;
    std::vector<VkBufferImageCopy> copies;
    VkDeviceSize offset = 0;
    uint32_t width = p.width, height = p.height;
    for (uint32_t i = 0; i < c.levels; ++i) {
      copies.push_back({.bufferOffset = offset,
                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1},
                        .imageExtent = {width, height, 1}});
      offset += VkDeviceSize((width + 3) / 4) * ((height + 3) / 4) * blockSize;
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }
    if (offset != c.upload.size())
      throw VK_ERROR_INITIALIZATION_FAILED;
    uploads.push_back(buffer(c.upload.data(), c.upload.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    copyLevels(command, value.data->image, c.levels, copies);
    // The staging buffer and then the image are now the only copies.
    release();
    return textures.emplace(source.get(), std::move(value)).first->second;
  }

  VkDescriptorSet RasterRenderer::Gpu::material(const std::shared_ptr<Material> &source,
                                                VkCommandBuffer command) {
    if (auto it = materials.find(source.get()); it != materials.end()) {
      return it->second->set;
    }
    auto &tex = texture(source->texture, command);
    auto value = std::make_unique<MaterialGpu>();
    value->device = device;
    value->pool = pool;
    value->owner = source;
    auto address = [](uint8_t mode) {
      switch (mode) {
      case 2:
        return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
      case 3:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
      case 4:
        return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
      default:
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
      }
    };
    const VkSamplerCreateInfo sampler = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                         .magFilter = source->filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                         .minFilter = source->filter ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
                                         .mipmapMode = source->filter ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                                      : VK_SAMPLER_MIPMAP_MODE_NEAREST,
                                         .addressModeU = address(source->wrapU),
                                         .addressModeV = address(source->wrapV),
                                         .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                                         .maxLod = float(source->texture->pixels.levels() - 1),
                                         .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    check(pipeline::createSampler(device, &sampler, nullptr, &value->sampler));
    const VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                    .descriptorPool = pool,
                                                    .descriptorSetCount = 1,
                                                    .pSetLayouts = &setLayout};
    check(vkAllocateDescriptorSets(device, &allocation, &value->set));
    const VkDescriptorImageInfo imageInfo = {value->sampler, tex.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    const VkWriteDescriptorSet write = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = value->set,
                                        .dstBinding = 0,
                                        .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                        .pImageInfo = &imageInfo};
    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    const auto set = value->set;
    materials.emplace(source.get(), std::move(value));
    return set;
  }

  VkDescriptorImageInfo RasterRenderer::sampledMaterial(const std::shared_ptr<Material> &source,
                                                        VkCommandBuffer command) {
    m_gpu->material(source, command);
    const auto &mat = *m_gpu->materials.at(source.get());
    return {mat.sampler, m_gpu->textures.at(source->texture.get()).view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }

  VkDescriptorImageInfo RasterRenderer::sampledEmission(const std::shared_ptr<Material> &source,
                                                        VkCommandBuffer command) {
    return sampledTexture(source, source->emissionTexture, command);
  }

  VkDescriptorImageInfo RasterRenderer::sampledTexture(const std::shared_ptr<Material> &source,
                                                       const std::shared_ptr<Texture> &image,
                                                       VkCommandBuffer command) {
    m_gpu->material(source, command);
    auto &texture = m_gpu->texture(image, command);
    return {m_gpu->materials.at(source.get())->sampler, texture.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
} // namespace remix
