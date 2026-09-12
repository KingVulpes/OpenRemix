#include "raster/gpu.h"
#include "raster/shaders/embedded.h"

namespace remix {
  using namespace scene;
  using namespace raster::detail;
  using namespace raster;

  RasterRenderer::Gpu::~Gpu() {
    destroyTarget();
    commands.clear();
    for (auto &frame : frames)
      frame = {};
    materials.clear();
    textures.clear();
    meshes.clear();
    uploads.clear();
    skin.reset();
    if (vertex) {
      vkDestroyShaderModule(device, vertex, nullptr);
    }
    if (fragment) {
      vkDestroyShaderModule(device, fragment, nullptr);
    }
    if (layout) {
      vkDestroyPipelineLayout(device, layout, nullptr);
    }
    if (pool) {
      vkDestroyDescriptorPool(device, pool, nullptr);
    }
    if (setLayout) {
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
    }
  }

  void RasterRenderer::Gpu::destroyTarget() {
    for (auto [key, pipeline] : pipelines) {
      vkDestroyPipeline(device, pipeline, nullptr);
    }
    pipelines.clear();
    depth.reset();
    pass = VK_NULL_HANDLE;
    extent = {};
  }

  RasterRenderer::RasterRenderer() { reset(); }

  RasterRenderer::~RasterRenderer() = default;

  void RasterRenderer::selectFrame(uint32_t slot) {
    auto &g = *m_gpu;
    auto &previous = g.frames[g.frameIndex];
    previous.uploads = std::move(g.uploads);
    previous.skin = std::move(g.skin);
    g.frameIndex = slot;
    auto &next = g.frames[slot];
    for (auto &buffer : next.retiredBuffers)
      g.recycle(std::move(buffer));
    next.retiredBuffers.clear();
    next.retiredMaterials.clear();
    next.retiredImages.clear();
    g.uploads = std::move(next.uploads);
    g.skin = std::move(next.skin);
  }

  void RasterRenderer::initializeGpu(VkPhysicalDevice physical, VkDevice device) {
    m_gpu = std::make_unique<Gpu>();
    auto &g = *m_gpu;
    g.device = device;
    g.physical = physical;
    vkGetPhysicalDeviceMemoryProperties(physical, &g.memory);
    vkGetPhysicalDeviceProperties(physical, &g.properties);
    for (auto format : {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM}) {
      VkFormatProperties p;
      vkGetPhysicalDeviceFormatProperties(physical, format, &p);
      if (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) {
        g.depthFormat = format;
        break;
      }
    }
    if (g.depthFormat == VK_FORMAT_UNDEFINED) {
      throw VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    const VkDescriptorSetLayoutBinding binding = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                                  VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    const VkDescriptorSetLayoutCreateInfo set = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = 1,
                                                 .pBindings = &binding};
    check(vkCreateDescriptorSetLayout(device, &set, nullptr, &g.setLayout));
    const VkDescriptorPoolSize size = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16384};
    const VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                             .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                             .maxSets = 16384,
                                             .poolSizeCount = 1,
                                             .pPoolSizes = &size};
    check(vkCreateDescriptorPool(device, &pool, nullptr, &g.pool));
    const VkPushConstantRange push = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                      sizeof(Push)};
    const VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                               .setLayoutCount = 1,
                                               .pSetLayouts = &g.setLayout,
                                               .pushConstantRangeCount = 1,
                                               .pPushConstantRanges = &push};
    check(vkCreatePipelineLayout(device, &layout, nullptr, &g.layout));
    VkShaderModuleCreateInfo shader = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                       .codeSize = sizeof(vertShader),
                                       .pCode = vertShader};
    check(vkCreateShaderModule(device, &shader, nullptr, &g.vertex));
    shader.codeSize = sizeof(fragShader);
    shader.pCode = fragShader;
    check(vkCreateShaderModule(device, &shader, nullptr, &g.fragment));
  }

  void RasterRenderer::configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput) {
    auto &g = *m_gpu;
    g.destroyTarget();
    g.pass = pass;
    g.skyPass = VK_NULL_HANDLE;
    g.extent = extent;
    g.encodeOutput = encodeOutput;
    g.depth = g.image(extent.width, extent.height, 1, g.depthFormat, true);
  }

  void RasterRenderer::setSkyTarget(VkRenderPass pass) {
    auto &g = *m_gpu;
    if (g.skyPass == pass)
      return;
    for (auto [key, pipeline] : g.pipelines)
      vkDestroyPipeline(g.device, pipeline, nullptr);
    g.pipelines.clear();
    g.skyPass = pass;
  }
  void RasterRenderer::destroyTarget() {
    if (m_gpu) {
      m_gpu->destroyTarget();
    }
  }

  void RasterRenderer::shutdownGpu() {
    m_gpu.reset();
    reset();
  }

  VkFormat RasterRenderer::depthFormat() const { return m_gpu->depthFormat; }

  VkImageView RasterRenderer::depthView() const { return m_gpu->depth->view; }

} // namespace remix
