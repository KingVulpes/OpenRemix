#include "upscale/renderer.h"
#include "upscale/sdk_backend.h"
#include "upscale/shaders/embedded.h"
#include <FidelityFX/host/backends/vk/ffx_vk.h>
#include <FidelityFX/host/ffx_fsr3upscaler.h>
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <vector>

namespace remix::upscale {
namespace {
FfxGetDeviceCapabilitiesFunc sdkCapabilities{};
void check(VkResult r) {
  if (r != VK_SUCCESS)
    throw r;
}
void checkFfx(FfxErrorCode r) {
  if (r != FFX_OK)
    throw std::runtime_error("FSR 3.1 error " + std::to_string(r));
}
struct Image {
  VkDevice device{};
  VkImage image{};
  VkImageView view{};
  VkDeviceMemory memory{};
  VkExtent2D extent{};
  VkFormat format{};
  ~Image() {
    if (view)
      vkDestroyImageView(device, view, nullptr);
    if (image)
      vkDestroyImage(device, image, nullptr);
    if (memory)
      vkFreeMemory(device, memory, nullptr);
  }
};
void barrier(VkCommandBuffer cmd) {
  VkMemoryBarrier b{
      .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
      .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
      .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT};
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, nullptr,
                       0, nullptr);
}
FfxResource resource(VkImage image, VkFormat format, VkExtent2D extent,
                     const wchar_t *name) {
  FfxResourceDescription desc{};
  desc.type = FFX_RESOURCE_TYPE_TEXTURE2D;
  desc.format = ffxGetSurfaceFormatVK(format);
  desc.width = extent.width;
  desc.height = extent.height;
  desc.depth = 1;
  desc.mipCount = 1;
  desc.usage = FFX_RESOURCE_USAGE_UAV;
  return ffxGetResourceVK(reinterpret_cast<void *>(image), desc, name,
                          FFX_RESOURCE_STATE_UNORDERED_ACCESS);
}
FfxResource resource(const Image &image, const wchar_t *name) {
  return resource(image.image, image.format, image.extent, name);
}
} // namespace
struct Renderer::Gpu {
  VkDevice device{};
  VkPhysicalDevice physical{};
  VkPhysicalDeviceMemoryProperties memory{};
  VkExtent2D render{}, display{};
  struct alignas(32) ScratchBlock {
    uint64_t words[4]{};
  };
  std::vector<ScratchBlock> scratch;
  FfxFsr3UpscalerContext context{};
  bool created{}, initialized{};
  std::unique_ptr<Image> depth, reactive, composition, output, dilatedDepth,
      dilatedMotion, previousDepth;
  VkDescriptorSetLayout setLayout{};
  VkDescriptorPool pool{};
  VkDescriptorSet set{};
  VkPipelineLayout layout{};
  VkPipeline prepare{};
  VkDescriptorSetLayout compositeSetLayout{};
  VkDescriptorPool compositePool{};
  VkDescriptorSet compositeSet{};
  VkPipelineLayout compositeLayout{};
  VkPipeline compositePipeline{};
  VkQueryPool timestamps{};
  uint32_t timestampBits{};
  float timestampPeriod{};
  Statistics stats;
  ~Gpu() {
    if (created)
      ffxFsr3UpscalerContextDestroy(&context);
    if (compositePipeline)
      vkDestroyPipeline(device, compositePipeline, nullptr);
    if (compositeLayout)
      vkDestroyPipelineLayout(device, compositeLayout, nullptr);
    if (compositePool)
      vkDestroyDescriptorPool(device, compositePool, nullptr);
    if (compositeSetLayout)
      vkDestroyDescriptorSetLayout(device, compositeSetLayout, nullptr);
    if (prepare)
      vkDestroyPipeline(device, prepare, nullptr);
    if (layout)
      vkDestroyPipelineLayout(device, layout, nullptr);
    if (pool)
      vkDestroyDescriptorPool(device, pool, nullptr);
    if (setLayout)
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    if (timestamps)
      vkDestroyQueryPool(device, timestamps, nullptr);
  }
  std::unique_ptr<Image> image(VkFormat format, VkExtent2D extent) {
    auto out = std::make_unique<Image>();
    out->device = device;
    out->format = format;
    out->extent = extent;
    VkImageCreateInfo info{.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                           .imageType = VK_IMAGE_TYPE_2D,
                           .format = format,
                           .extent = {extent.width, extent.height, 1},
                           .mipLevels = 1,
                           .arrayLayers = 1,
                           .samples = VK_SAMPLE_COUNT_1_BIT,
                           .tiling = VK_IMAGE_TILING_OPTIMAL,
                           .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                    VK_IMAGE_USAGE_SAMPLED_BIT |
                                    VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                           .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateImage(device, &info, nullptr, &out->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, out->image, &req);
    uint32_t type = 0;
    for (; type < memory.memoryTypeCount; ++type)
      if ((req.memoryTypeBits & (1u << type)) &&
          (memory.memoryTypes[type].propertyFlags &
           VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
        break;
    if (type == memory.memoryTypeCount)
      throw std::runtime_error("No FSR image memory type");
    VkMemoryAllocateInfo alloc{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                               .allocationSize = req.size,
                               .memoryTypeIndex = type};
    check(vkAllocateMemory(device, &alloc, nullptr, &out->memory));
    check(vkBindImageMemory(device, out->image, out->memory, 0));
    VkImageViewCreateInfo view{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = out->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(device, &view, nullptr, &out->view));
    return out;
  }
};
Renderer::Renderer() : m_gpu(std::make_unique<Gpu>()) {}
Renderer::~Renderer() = default;
bool supportsCamera(const scene::Matrix &projection) {
  if (std::abs(projection[11]) < .0001f || std::abs(projection[15]) > .0001f ||
      !projection[5])
    return false;
  const auto inverse = scene::inverse(projection);
  const float near = std::abs(inverse[14] / inverse[15]);
  const float far =
      std::abs((inverse[10] + inverse[14]) / (inverse[11] + inverse[15]));
  return std::isfinite(near) && std::isfinite(far) && near > 0 && far > near;
}
VkExtent2D renderSize(VkExtent2D display, uint32_t quality) {
  VkExtent2D out{};
  checkFfx(ffxFsr3UpscalerGetRenderResolutionFromQualityMode(
      &out.width, &out.height, display.width, display.height,
      FfxFsr3UpscalerQualityMode(std::min(quality, 4u))));
  out.width = std::max(out.width, 1u);
  out.height = std::max(out.height, 1u);
  return out;
}
void jitter(float *xy, uint64_t frame, VkExtent2D render, VkExtent2D display) {
  int32_t phase =
      ffxFsr3UpscalerGetJitterPhaseCount(render.width, display.width);
  checkFfx(ffxFsr3UpscalerGetJitterOffset(xy, xy + 1, int32_t(frame % phase),
                                          phase));
}
void Renderer::initialize(VkPhysicalDevice physical, VkDevice device,
                          uint32_t queueFamily, VkExtent2D render,
                          VkExtent2D display) {
  auto &g = *m_gpu;
  g.device = device;
  g.physical = physical;
  g.render = render;
  g.display = display;
  vkGetPhysicalDeviceMemoryProperties(physical, &g.memory);
  VkPhysicalDeviceFeatures features{};
  vkGetPhysicalDeviceFeatures(physical, &features);
  VkPhysicalDeviceSubgroupProperties subgroup{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES};
  VkPhysicalDeviceProperties2 properties{
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
      .pNext = &subgroup};
  vkGetPhysicalDeviceProperties2(physical, &properties);
  if (!features.shaderStorageImageWriteWithoutFormat ||
      !(subgroup.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT) ||
      !(subgroup.supportedOperations & VK_SUBGROUP_FEATURE_QUAD_BIT))
    throw std::runtime_error(
        "FSR requires compute subgroup quads and unformatted image writes");
  for (auto format :
       {VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R32_UINT}) {
    VkFormatProperties props{};
    vkGetPhysicalDeviceFormatProperties(physical, format, &props);
    const auto required = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((props.optimalTilingFeatures & required) != required)
      throw std::runtime_error("FSR sampled/storage image formats unavailable");
  }
  VkDeviceContext vkContext{device, physical, sdkDeviceProc};
  FfxFsr3UpscalerContextDescription desc{};
  desc.flags = FFX_FSR3UPSCALER_ENABLE_HIGH_DYNAMIC_RANGE;
  desc.maxRenderSize = {render.width, render.height};
  desc.maxUpscaleSize = {display.width, display.height};
  g.scratch.resize((ffxGetScratchMemorySizeVK(physical, 1) + 31) / 32);
  checkFfx(ffxGetInterfaceVK(&desc.backendInterface, ffxGetDeviceVK(&vkContext),
                             g.scratch.data(), g.scratch.size() * 32, 1));
  // The SDK queries physical FP16 support, but it may not be enabled on our
  // logical device. Select the portable full-precision shader contract
  // explicitly.
  sdkCapabilities = desc.backendInterface.fpGetDeviceCapabilities;
  desc.backendInterface.fpGetDeviceCapabilities =
      [](FfxInterface *backend, FfxDeviceCapabilities *caps) {
        auto result = sdkCapabilities(backend, caps);
        caps->waveLaneCountMax = caps->waveLaneCountMin;
        caps->fp16Supported = false;
        return result;
      };
  ContextAllocations allocations;
  checkFfx(ffxFsr3UpscalerContextCreate(&g.context, &desc));
  g.created = true;
  allocations.commit();
  g.depth = g.image(VK_FORMAT_R32_SFLOAT, render);
  g.reactive = g.image(VK_FORMAT_R8_UNORM, render);
  g.composition = g.image(VK_FORMAT_R8_UNORM, render);
  g.output = g.image(VK_FORMAT_R16G16B16A16_SFLOAT, display);
  g.dilatedDepth = g.image(VK_FORMAT_R32_SFLOAT, render);
  g.dilatedMotion = g.image(VK_FORMAT_R16G16_SFLOAT, render);
  g.previousDepth = g.image(VK_FORMAT_R32_UINT, render);
  VkDescriptorSetLayoutBinding bindings[5]{};
  for (unsigned i = 0; i < 5; ++i)
    bindings[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
  VkDescriptorSetLayoutCreateInfo si{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 5,
      .pBindings = bindings};
  check(vkCreateDescriptorSetLayout(device, &si, nullptr, &g.setLayout));
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 5};
  VkDescriptorPoolCreateInfo pi{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &size};
  check(vkCreateDescriptorPool(device, &pi, nullptr, &g.pool));
  VkDescriptorSetAllocateInfo ai{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = g.pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &g.setLayout};
  check(vkAllocateDescriptorSets(device, &ai, &g.set));
  VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0,
                           sizeof(scene::Matrix)};
  VkPipelineLayoutCreateInfo li{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &g.setLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push};
  check(vkCreatePipelineLayout(device, &li, nullptr, &g.layout));
  VkShaderModuleCreateInfo mi{.sType =
                                  VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                              .codeSize = sizeof(compShader),
                              .pCode = compShader};
  VkShaderModule module{};
  check(vkCreateShaderModule(device, &mi, nullptr, &module));
  VkComputePipelineCreateInfo ci{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main"},
      .layout = g.layout};
  auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &ci,
                                         nullptr, &g.prepare);
  vkDestroyShaderModule(device, module, nullptr);
  check(result);
  uint32_t count = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
  std::vector<VkQueueFamilyProperties> families(count);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
  g.timestampBits = families.at(queueFamily).timestampValidBits;
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(physical, &props);
  g.timestampPeriod = props.limits.timestampPeriod;
  if (g.timestampBits) {
    VkQueryPoolCreateInfo qi{.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                             .queryType = VK_QUERY_TYPE_TIMESTAMP,
                             .queryCount = 2};
    check(vkCreateQueryPool(device, &qi, nullptr, &g.timestamps));
  }
  g.stats.active = true;
  g.stats.render = render;
  g.stats.display = display;
}

Statistics Renderer::statistics() const { return m_gpu->stats; }
void Renderer::record(VkCommandBuffer command, Input color, Input depth,
                      Input motion, Input transmission,
                      const scene::Matrix &projection, const float *jitter,
                      float milliseconds, float sharpness, bool reset) {
  auto &g = *m_gpu;
  if (g.timestamps) {
    uint64_t times[2]{};
    if (g.stats.frames &&
        vkGetQueryPoolResults(g.device, g.timestamps, 0, 2, sizeof(times),
                              times, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
      uint64_t mask = g.timestampBits == 64
                          ? ~uint64_t(0)
                          : (uint64_t(1) << g.timestampBits) - 1;
      g.stats.milliseconds =
          float((times[1] - times[0]) & mask) * g.timestampPeriod / 1e6f;
    }
    vkCmdResetQueryPool(command, g.timestamps, 0, 2);
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        g.timestamps, 0);
  }
  barrier(command);
  if (!g.initialized) {
    for (auto image :
         {g.depth.get(), g.reactive.get(), g.composition.get(), g.output.get(),
          g.dilatedDepth.get(), g.dilatedMotion.get(), g.previousDepth.get()}) {
      VkImageMemoryBarrier b{
          .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask =
              VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_GENERAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .image = image->image,
          .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                           0, nullptr, 1, &b);
    }
  }
  VkDescriptorImageInfo images[5]{};
  VkImageView views[] = {depth.view, transmission.view, g.depth->view,
                         g.reactive->view, g.composition->view};
  VkWriteDescriptorSet writes[5]{};
  for (unsigned i = 0; i < 5; ++i) {
    images[i] = {VK_NULL_HANDLE, views[i], VK_IMAGE_LAYOUT_GENERAL};
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = g.set,
                 .dstBinding = i,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &images[i]};
  }
  vkUpdateDescriptorSets(g.device, 5, writes, 0, nullptr);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.prepare);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE, g.layout, 0,
                          1, &g.set, 0, nullptr);
  vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                     sizeof(projection), &projection);
  vkCmdDispatch(command, (g.render.width + 7) / 8, (g.render.height + 7) / 8,
                1);
  barrier(command);
  FfxFsr3UpscalerDispatchDescription desc{};
  desc.commandList = ffxGetCommandListVK(command);
  desc.color = resource(color.image, VK_FORMAT_R16G16B16A16_SFLOAT, g.render,
                        L"Denoised HDR");
  desc.depth = resource(*g.depth, L"Device depth");
  desc.motionVectors = resource(motion.image, VK_FORMAT_R32G32B32A32_SFLOAT,
                                g.render, L"Unjittered pixel motion");
  desc.reactive = resource(*g.reactive, L"Reactive");
  desc.transparencyAndComposition = resource(*g.composition, L"Composition");
  desc.dilatedDepth = resource(*g.dilatedDepth, L"Dilated depth");
  desc.dilatedMotionVectors = resource(*g.dilatedMotion, L"Dilated motion");
  desc.reconstructedPrevNearestDepth =
      resource(*g.previousDepth, L"Previous depth");
  desc.output = resource(*g.output, L"Upscaled HDR");
  // Rays sample pixel + jitter; FSR subtracts its projected-geometry jitter
  // when locating that sample in the unjittered image.
  desc.jitterOffset = {-jitter[0], -jitter[1]};
  desc.motionVectorScale = {1, 1};
  desc.renderSize = {g.render.width, g.render.height};
  desc.upscaleSize = {g.display.width, g.display.height};
  desc.enableSharpening = sharpness > 0;
  desc.sharpness = sharpness;
  desc.frameTimeDelta = milliseconds;
  desc.preExposure = 1;
  desc.reset = reset || !g.initialized;
  auto inverse = scene::inverse(projection);
  desc.cameraNear = std::abs(inverse[14] / inverse[15]);
  desc.cameraFar =
      std::abs((inverse[10] + inverse[14]) / (inverse[11] + inverse[15]));
  desc.cameraFovAngleVertical = 2 * std::atan(1 / std::abs(projection[5]));
  desc.viewSpaceToMetersFactor = 1;
  checkFfx(ffxFsr3UpscalerContextDispatch(&g.context, &desc));
  barrier(command);
  if (g.timestamps)
    vkCmdWriteTimestamp(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        g.timestamps, 1);
  ++g.stats.frames;
  if (desc.reset)
    ++g.stats.resets;
  g.initialized = true;
}
void Renderer::configureComposite(VkRenderPass pass, VkImageView stableDepth) {
  auto &g = *m_gpu;
  VkDescriptorSetLayoutBinding bindings[] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
       nullptr}};
  VkDescriptorSetLayoutCreateInfo si{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 2,
      .pBindings = bindings};
  check(vkCreateDescriptorSetLayout(g.device, &si, nullptr,
                                    &g.compositeSetLayout));
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2};
  VkDescriptorPoolCreateInfo pool{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 1,
      .pPoolSizes = &size};
  check(vkCreateDescriptorPool(g.device, &pool, nullptr, &g.compositePool));
  VkDescriptorSetAllocateInfo ai{
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = g.compositePool,
      .descriptorSetCount = 1,
      .pSetLayouts = &g.compositeSetLayout};
  check(vkAllocateDescriptorSets(g.device, &ai, &g.compositeSet));
  VkDescriptorImageInfo images[] = {
      {VK_NULL_HANDLE, g.output->view, VK_IMAGE_LAYOUT_GENERAL},
      {VK_NULL_HANDLE, stableDepth, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet writes[2]{};
  for (unsigned i = 0; i < 2; ++i)
    writes[i] = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                 .dstSet = g.compositeSet,
                 .dstBinding = i,
                 .descriptorCount = 1,
                 .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                 .pImageInfo = &images[i]};
  vkUpdateDescriptorSets(g.device, 2, writes, 0, nullptr);
  VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
  VkPipelineLayoutCreateInfo li{
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &g.compositeSetLayout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &push};
  check(vkCreatePipelineLayout(g.device, &li, nullptr, &g.compositeLayout));
  VkShaderModule modules[2]{};
  try {
    const uint32_t *codes[] = {vertShader, fragShader};
    const size_t sizes[] = {sizeof(vertShader), sizeof(fragShader)};
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (unsigned i = 0; i < 2; ++i) {
      VkShaderModuleCreateInfo mi{
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
          .codeSize = sizes[i],
          .pCode = codes[i]};
      check(vkCreateShaderModule(g.device, &mi, nullptr, &modules[i]));
      stages[i] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT
                              : VK_SHADER_STAGE_VERTEX_BIT,
                   .module = modules[i],
                   .pName = "main"};
    }
    VkPipelineVertexInputStateCreateInfo vertex{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .maxDepthBounds = 1};
    VkPipelineColorBlendAttachmentState attachment{.colorWriteMask = 15};
    VkPipelineColorBlendStateCreateInfo blend{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &attachment};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT,
                               VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = states};
    VkGraphicsPipelineCreateInfo ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vertex,
        .pInputAssemblyState = &assembly,
        .pViewportState = &viewport,
        .pRasterizationState = &raster,
        .pMultisampleState = &samples,
        .pDepthStencilState = &depth,
        .pColorBlendState = &blend,
        .pDynamicState = &dynamic,
        .layout = g.compositeLayout,
        .renderPass = pass};
    check(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &ci, nullptr,
                                    &g.compositePipeline));
  } catch (...) {
    for (auto module : modules)
      if (module)
        vkDestroyShaderModule(g.device, module, nullptr);
    throw;
  }
  for (auto module : modules)
    vkDestroyShaderModule(g.device, module, nullptr);
}
void Renderer::composite(VkCommandBuffer command, bool encodeOutput, bool prism, bool gamma22, bool hardwareSrgb) {
  const auto &g = *m_gpu;
  VkViewport viewport{0, 0, float(g.display.width), float(g.display.height),
                      0, 1};
  VkRect2D scissor{{0, 0}, g.display};
  vkCmdSetViewport(command, 0, 1, &viewport);
  vkCmdSetScissor(command, 0, 1, &scissor);
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    g.compositePipeline);
  vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          g.compositeLayout, 0, 1, &g.compositeSet, 0, nullptr);
  const uint32_t settings[] = {uint32_t(encodeOutput), uint32_t(prism), uint32_t(gamma22), uint32_t(hardwareSrgb)};
  vkCmdPushConstants(command, g.compositeLayout, VK_SHADER_STAGE_FRAGMENT_BIT,
                     0, sizeof(settings), settings);
  vkCmdDraw(command, 3, 1, 0, 0);
}

} // namespace remix::upscale
