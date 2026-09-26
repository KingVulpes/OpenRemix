#include "raster/gpu.h"

namespace remix {
  using namespace scene;
  using namespace raster::detail;

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
    const auto &p = source->pixels;
    const uint32_t levels = uint32_t(p.mips.size());
    TextureGpu value{source, image(p.width, p.height, levels,
                                   !p.hdrMips.empty() ? VK_FORMAT_R32G32B32A32_SFLOAT
                                   : p.srgb           ? VK_FORMAT_R8G8B8A8_SRGB
                                                      : VK_FORMAT_R8G8B8A8_UNORM,
                                   false)};
    std::vector<uint8_t> data;
    std::vector<VkBufferImageCopy> copies;
    uint32_t width = p.width, height = p.height;
    for (uint32_t i = 0; i < levels; ++i) {
      copies.push_back({.bufferOffset = data.size(),
                        .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1},
                        .imageExtent = {width, height, 1}});
      if (p.hdrMips.empty())
        data.insert(data.end(), p.mips[i].begin(), p.mips[i].end());
      else {
        const auto *begin = reinterpret_cast<const uint8_t *>(p.hdrMips[i].data());
        data.insert(data.end(), begin, begin + p.hdrMips[i].size() * sizeof(float));
      }
      width = std::max(1u, width / 2);
      height = std::max(1u, height / 2);
    }
    uploads.push_back(buffer(data.data(), data.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT));
    VkImageMemoryBarrier barrier = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = value.data->image,
                                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1}};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    vkCmdCopyBufferToImage(command, uploads.back()->buffer, value.data->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, levels, copies.data());
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
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
                                         .maxLod = float(source->texture->pixels.mips.size() - 1),
                                         .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK};
    check(vkCreateSampler(device, &sampler, nullptr, &value->sampler));
    const VkDescriptorSetAllocateInfo allocation = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                                    .descriptorPool = pool,
                                                    .descriptorSetCount = 1,
                                                    .pSetLayouts = &setLayout};
    check(vkAllocateDescriptorSets(device, &allocation, &value->set));
    const VkDescriptorImageInfo imageInfo = {value->sampler, tex.data->view,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
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
    return {mat.sampler, m_gpu->textures.at(source->texture.get()).data->view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
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
    return {m_gpu->materials.at(source.get())->sampler, texture.data->view,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  }
} // namespace remix
