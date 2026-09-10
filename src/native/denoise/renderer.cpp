#include "denoise/gpu.h"
#include "denoise/shaders/embedded.h"
#include <algorithm>
#include <cstring>

namespace remix::denoise {
  remixapi_DenoisingCapabilitiesEXT capabilities(VkPhysicalDevice physical) {
    remixapi_DenoisingCapabilitiesEXT result{.structSize = sizeof(result)};
    VkPhysicalDeviceFeatures f;
    vkGetPhysicalDeviceFeatures(physical, &f);
    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(physical, &p);
    std::string reason;
    auto require = [&](bool ok, const char *name) {
      if (!ok) {
        if (!reason.empty())
          reason += ", ";
        reason += name;
      }
    };
    require(p.apiVersion >= VK_API_VERSION_1_2, "Vulkan 1.2");
    require(f.shaderStorageImageExtendedFormats, "extended storage-image formats");
    // The temporal set is used alongside the tracer set (eight storage images,
    // 512 textures, two acceleration structures, a storage buffer and a UBO).
    require(p.limits.maxPerStageDescriptorStorageImages >= ImageCount + 11 &&
                p.limits.maxDescriptorSetStorageImages >= ImageCount + 11,
            "temporal storage-image descriptor limit");
    require(p.limits.maxPerStageResources >= 512 + ImageCount + 16, "temporal per-stage resource limit");
    const VkFormat formats[] = {VK_FORMAT_R16G16B16A16_SFLOAT,
                                VK_FORMAT_R32G32B32A32_SFLOAT,
                                VK_FORMAT_R32_SFLOAT,
                                VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                                VK_FORMAT_R8_UNORM,
                                VK_FORMAT_R8_UINT,
                                VK_FORMAT_R16_UINT,
                                VK_FORMAT_R16_SFLOAT};
    for (auto format : formats) {
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
      require((fp.optimalTilingFeatures &
               (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) ==
                  (VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT),
              "NRD sampled/storage format");
    }
    for (auto format : {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_A2B10G10R10_UNORM_PACK32,
                        VK_FORMAT_R16_SFLOAT, VK_FORMAT_R32_SFLOAT, VK_FORMAT_R8_UNORM}) {
      VkFormatProperties fp;
      vkGetPhysicalDeviceFormatProperties(physical, format, &fp);
      require(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT,
              "temporal linear texture filtering");
    }
    result.taaSupported = reason.empty();
    require(f.shaderStorageImageReadWithoutFormat && f.shaderStorageImageWriteWithoutFormat,
            "storage-image access without format");
#ifndef REMIX_HAS_NRD
    require(false, "NRD disabled at build time");
#endif
    result.reblurShSupported = reason.empty();
    std::strncpy(result.unavailableReason, reason.c_str(), sizeof(result.unavailableReason) - 1);
    return result;
  }
  uint32_t Renderer::Gpu::memoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (memory.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw VK_ERROR_FEATURE_NOT_PRESENT;
  }
  std::unique_ptr<Texture> Renderer::Gpu::texture(VkFormat format, VkExtent2D size) {
    auto t = std::make_unique<Texture>();
    t->device = device;
    t->extent = size;
    VkImageCreateInfo ci{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                         .imageType = VK_IMAGE_TYPE_2D,
                         .format = format,
                         .extent = {size.width, size.height, 1},
                         .mipLevels = 1,
                         .arrayLayers = 1,
                         .samples = VK_SAMPLE_COUNT_1_BIT,
                         .tiling = VK_IMAGE_TILING_OPTIMAL,
                         .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                  VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
    check(vkCreateImage(device, &ci, nullptr, &t->image));
    VkMemoryRequirements r;
    vkGetImageMemoryRequirements(device, t->image, &r);
    VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                            .allocationSize = r.size,
                            .memoryTypeIndex =
                                memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(device, &ai, nullptr, &t->memory));
    check(vkBindImageMemory(device, t->image, t->memory, 0));
    t->bytes = r.size;
    VkImageViewCreateInfo vi{.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                             .image = t->image,
                             .viewType = VK_IMAGE_VIEW_TYPE_2D,
                             .format = format,
                             .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(device, &vi, nullptr, &t->view));
    stats.memoryBytes += r.size;
    return t;
  }
  std::unique_ptr<Buffer> Renderer::Gpu::buffer(VkDeviceSize size) {
    auto b = std::make_unique<Buffer>();
    b->device = device;
    b->size = size;
    VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                          .size = size,
                          .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    check(vkCreateBuffer(device, &ci, nullptr, &b->buffer));
    VkMemoryRequirements r;
    vkGetBufferMemoryRequirements(device, b->buffer, &r);
    VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                            .allocationSize = r.size,
                            .memoryTypeIndex =
                                memoryType(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    check(vkAllocateMemory(device, &ai, nullptr, &b->memory));
    check(vkBindBufferMemory(device, b->buffer, b->memory, 0));
    check(vkMapMemory(device, b->memory, 0, size, 0, &b->mapped));
    stats.memoryBytes += r.size;
    return b;
  }
  VkPipeline Renderer::Gpu::pipeline(VkPipelineLayout pl, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule module{};
    check(vkCreateShaderModule(device, &ci, nullptr, &module));
    VkComputePipelineCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                   .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                             .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                             .module = module,
                                             .pName = "main"},
                                   .layout = pl};
    VkPipeline result{};
    auto status = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &result);
    vkDestroyShaderModule(device, module, nullptr);
    check(status);
    return result;
  }
  Renderer::Gpu::~Gpu() {
#ifdef REMIX_HAS_NRD
    for (auto &p : pipelines) {
      if (p.pipeline)
        vkDestroyPipeline(device, p.pipeline, nullptr);
      if (p.layout)
        vkDestroyPipelineLayout(device, p.layout, nullptr);
      if (p.resources)
        vkDestroyDescriptorSetLayout(device, p.resources, nullptr);
    }
    if (nrdPool)
      vkDestroyDescriptorPool(device, nrdPool, nullptr);
    if (commonLayout)
      vkDestroyDescriptorSetLayout(device, commonLayout, nullptr);
    for (auto sampler : samplers)
      if (sampler)
        vkDestroySampler(device, sampler, nullptr);
    if (instance)
      nrd::DestroyInstance(*instance);
#endif
    if (timestamps)
      vkDestroyQueryPool(device, timestamps, nullptr);
    if (post)
      vkDestroyPipeline(device, post, nullptr);
    if (layout)
      vkDestroyPipelineLayout(device, layout, nullptr);
    if (pool)
      vkDestroyDescriptorPool(device, pool, nullptr);
    if (inputLayout)
      vkDestroyDescriptorSetLayout(device, inputLayout, nullptr);
    if (emptyLayout)
      vkDestroyDescriptorSetLayout(device, emptyLayout, nullptr);
  }
  Renderer::Renderer() = default;
  Renderer::~Renderer() = default;
  void Renderer::initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent, bool reblur,
                            uint32_t queueFamily) {
    m_gpu = std::make_unique<Gpu>();
    auto &g = *m_gpu;
    g.physical = physical;
    g.device = device;
    g.extent = extent;
    vkGetPhysicalDeviceMemoryProperties(physical, &g.memory);
    vkGetPhysicalDeviceProperties(physical, &g.properties);
    if (extent.width > 65535 || extent.height > 65535)
      throw VK_ERROR_FORMAT_NOT_SUPPORTED;
    for (uint32_t i = 0; i < ImageCount; ++i) {
      VkFormat format = VK_FORMAT_R16G16B16A16_SFLOAT;
      if (i == Normal || i == PreviousNormal)
        format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
      if (i == Depth || i == PreviousDepth || i == Confidence)
        format = VK_FORMAT_R32_SFLOAT;
      if (i == Motion)
        format = VK_FORMAT_R32G32B32A32_SFLOAT;
      VkExtent2D size = extent;
      if (i == GradientA || i == GradientB)
        size = {(extent.width + 4) / 5, (extent.height + 4) / 5};
      g.images[i] = g.texture(format, size);
    }
    g.uniform = g.buffer(sizeof(Frame));
    std::vector<VkDescriptorSetLayoutBinding> bindings;
    for (uint32_t i = 0; i < ImageCount + 3; ++i)
      bindings.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    bindings.push_back(
        {ImageCount + 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo ci{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    check(vkCreateDescriptorSetLayout(device, &ci, nullptr, &g.emptyLayout));
    ci.bindingCount = uint32_t(bindings.size());
    ci.pBindings = bindings.data();
    check(vkCreateDescriptorSetLayout(device, &ci, nullptr, &g.inputLayout));
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ImageCount + 3},
                                    {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pc{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                  .maxSets = 1,
                                  .poolSizeCount = 2,
                                  .pPoolSizes = sizes};
    check(vkCreateDescriptorPool(device, &pc, nullptr, &g.pool));
    VkDescriptorSetAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                   .descriptorPool = g.pool,
                                   .descriptorSetCount = 1,
                                   .pSetLayouts = &g.inputLayout};
    check(vkAllocateDescriptorSets(device, &ai, &g.inputs));
    VkDescriptorSetLayout layouts[] = {g.emptyLayout, g.inputLayout};
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
    VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                  .setLayoutCount = 2,
                                  .pSetLayouts = layouts,
                                  .pushConstantRangeCount = 1,
                                  .pPushConstantRanges = &push};
    check(vkCreatePipelineLayout(device, &li, nullptr, &g.layout));
    g.post = g.pipeline(g.layout, compShader, sizeof(compShader));
#ifdef REMIX_HAS_NRD
    if (reblur)
      g.initializeNrd();
#else
    (void)reblur;
#endif
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> queues(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, queues.data());
    g.timestampBits = queues.at(queueFamily).timestampValidBits;
    if (g.timestampBits && g.properties.limits.timestampComputeAndGraphics) {
      VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                               .queryType = VK_QUERY_TYPE_TIMESTAMP,
                               .queryCount = 5};
      check(vkCreateQueryPool(device, &qi, nullptr, &g.timestamps));
    }
    g.stats.peakMemoryBytes = g.stats.memoryBytes;
  }
  VkDescriptorSetLayout Renderer::inputLayout() const { return m_gpu->inputLayout; }
  VkDescriptorSet Renderer::inputs() const { return m_gpu->inputs; }
  void Renderer::begin(VkCommandBuffer command, const Frame &frame, const scene::Matrix &projection,
                       const scene::Matrix &previousProjection, VkImageView world, VkImageView sky) {
    auto &g = *m_gpu;
    g.frame = frame;
    g.frame.flags[0] |= !g.initialized;
    if (g.timingPending) {
      uint64_t values[5];
      auto r = vkGetQueryPoolResults(g.device, g.timestamps, 0, 5, sizeof(values), values, 8,
                                     VK_QUERY_RESULT_64_BIT);
      if (r == VK_ERROR_DEVICE_LOST)
        throw r;
      if (r == VK_SUCCESS) {
        const uint64_t mask = g.timestampBits == 64 ? UINT64_MAX : (uint64_t(1) << g.timestampBits) - 1;
        g.stats.gpuConfidenceMilliseconds =
            float((values[1] - values[0]) & mask) * g.properties.limits.timestampPeriod / 1e6f;
        g.stats.gpuDenoiseMilliseconds =
            float((values[2] - values[1]) & mask) * g.properties.limits.timestampPeriod / 1e6f;
        g.stats.gpuTaaMilliseconds =
            float((values[4] - values[3]) & mask) * g.properties.limits.timestampPeriod / 1e6f;
        g.stats.gpuTimingsAvailable = 1;
      }
    }
    if (g.timestamps)
      vkCmdResetQueryPool(command, g.timestamps, 0, 5);
    if (!g.initialized) {
      std::vector<Texture *> textures;
      for (auto &t : g.images)
        textures.push_back(t.get());
#ifdef REMIX_HAS_NRD
      for (auto &t : g.permanent)
        textures.push_back(t.get());
      for (auto &t : g.transient)
        textures.push_back(t.get());
#endif
      for (auto *t : textures) {
        VkImageMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                               .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                               .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = t->image,
                               .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        VkClearColorValue zero{};
        vkCmdClearColorImage(command, t->image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &b.subresourceRange);
      }
      g.initialized = true;
    }
    synchronize(command);
    std::memcpy(g.uniform->mapped, &g.frame, sizeof(Frame));
    std::vector<VkDescriptorImageInfo> images;
    for (auto &t : g.images)
      images.push_back({VK_NULL_HANDLE, t->view, VK_IMAGE_LAYOUT_GENERAL});
    images.push_back({VK_NULL_HANDLE, world, VK_IMAGE_LAYOUT_GENERAL});
    images.push_back({VK_NULL_HANDLE, sky, VK_IMAGE_LAYOUT_GENERAL});
    images.push_back(images[0]);
    std::vector<VkWriteDescriptorSet> writes;
    for (uint32_t i = 0; i < images.size(); ++i)
      writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                        .dstSet = g.inputs,
                        .dstBinding = i,
                        .descriptorCount = 1,
                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                        .pImageInfo = &images[i]});
    VkDescriptorBufferInfo bi{g.uniform->buffer, 0, sizeof(Frame)};
    writes.push_back({.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                      .dstSet = g.inputs,
                      .dstBinding = ImageCount + 3,
                      .descriptorCount = 1,
                      .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                      .pBufferInfo = &bi});
    vkUpdateDescriptorSets(g.device, uint32_t(writes.size()), writes.data(), 0, nullptr);
#ifdef REMIX_HAS_NRD
    if (g.frame.flags[1])
      g.prepareNrd(projection, previousProjection);
#else
    (void)projection;
    (void)previousProjection;
#endif
  }
  void Renderer::Gpu::pass(VkCommandBuffer command, uint32_t mode, uint32_t step) {
    synchronize(command);
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, post);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 1, 1, &inputs, 0, nullptr);
    const uint32_t push[] = {mode, step};
    vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, 8, push);
    auto size = mode == 0 ? images[GradientA]->extent : extent;
    vkCmdDispatch(command, (size.width + 7) / 8, (size.height + 7) / 8, 1);
  }
  void Renderer::beginConfidence(VkCommandBuffer command) {
    if (m_gpu->timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_gpu->timestamps, 0);
  }
  void Renderer::record(VkCommandBuffer command) {
    auto &g = *m_gpu;
    if (g.frame.flags[1]) {
      for (uint32_t i = 1; i <= 5; ++i)
        g.pass(command, 0, i);
      g.pass(command, 1);
    }
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 1);
#ifdef REMIX_HAS_NRD
    if (g.frame.flags[1])
      g.recordNrd(command);
#endif
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 2);
    g.pass(command, 2);
    if (g.timestamps)
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 3);
    g.pass(command, 3);
    g.pass(command, 4);
    if (g.timestamps) {
      vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, g.timestamps, 4);
      g.timingPending = true;
    }
    synchronize(command);
    g.submittedFrame = true;
  }
  void Renderer::submitted() {
    auto &g = *m_gpu;
    if (!g.submittedFrame)
      return;
    g.submittedFrame = false;
    if (g.frame.flags[0]) {
      ++g.stats.historyResets;
      g.stats.historyFrames = 0;
    }
    ++g.stats.renderedFrames;
    ++g.stats.historyFrames;
    g.stats.activeDenoiser = g.frame.flags[1];
    g.stats.activeTaa = g.frame.flags[2];
    g.stats.validationEnabled = g.frame.timing[1] != 0;
    if (g.frame.flags[0] || g.frame.timing[2] != 0)
      g.quietFrames = 0;
    ++g.quietFrames;
    g.stats.redrawRequested = g.quietFrames < g.warmupFrames;
  }
  remixapi_DenoisingStatisticsEXT Renderer::statistics() const { return m_gpu->stats; }
} // namespace remix::denoise
