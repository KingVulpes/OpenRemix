#include "pathtrace/gpu.h"
#include "pathtrace/shaders/embedded.h"

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  PathTracer::Gpu::~Gpu() {
    destroyTarget();
    meshes.clear();
    compactPending.clear();
    retired.clear();
    for (auto &top : tlas) {
      top.accel.reset();
      top.instances.reset();
    }
    arenas.clear();
    if (timestamps)
      vkDestroyQueryPool(device, timestamps, nullptr);
    if (compaction)
      vkDestroyQueryPool(device, compaction, nullptr);
    if (compute)
      vkDestroyPipeline(device, compute, nullptr);
    for (auto shader : shaders)
      if (shader)
        vkDestroyShaderModule(device, shader, nullptr);
    if (layout)
      vkDestroyPipelineLayout(device, layout, nullptr);
    if (pool)
      vkDestroyDescriptorPool(device, pool, nullptr);
    if (setLayout)
      vkDestroyDescriptorSetLayout(device, setLayout, nullptr);
  }

  std::unique_ptr<Image> PathTracer::Gpu::image(bool colorAttachment, VkFormat format) {
    auto out = std::make_unique<Image>();
    out->device = device;
    VkImageCreateInfo info = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                              .imageType = VK_IMAGE_TYPE_2D,
                              .format = format,
                              .extent = {extent.width, extent.height, 1},
                              .mipLevels = 1,
                              .arrayLayers = 1,
                              .samples = VK_SAMPLE_COUNT_1_BIT,
                              .tiling = VK_IMAGE_TILING_OPTIMAL,
                              .usage = VK_IMAGE_USAGE_STORAGE_BIT |
                                       (colorAttachment ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0u),
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE};
    check(vkCreateImage(device, &info, nullptr, &out->handle));
    VkMemoryRequirements requirements;
    vkGetImageMemoryRequirements(device, out->handle, &requirements);
    VkMemoryAllocateInfo allocate = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = memoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    check(vkAllocateMemory(device, &allocate, nullptr, &out->memory));
    check(vkBindImageMemory(device, out->handle, out->memory, 0));
    VkImageViewCreateInfo view = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .image = out->handle,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = info.format,
                                  .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    check(vkCreateImageView(device, &view, nullptr, &out->view));
    return out;
  }

  void PathTracer::Gpu::destroyTarget() {
    destroyPost();
    destroyPrimary();
    history.clear();
    previousVertices.clear();
    pendingVertices.clear();
    retiredPreviousVertices.clear();
    if (skyFramebuffer)
      vkDestroyFramebuffer(device, skyFramebuffer, nullptr);
    if (skyPass)
      vkDestroyRenderPass(device, skyPass, nullptr);
    skyFramebuffer = VK_NULL_HANDLE;
    skyPass = VK_NULL_HANDLE;
    sky.reset();
    for (auto &pipeline : composite) {
      if (pipeline)
        vkDestroyPipeline(device, pipeline, nullptr);
      pipeline = VK_NULL_HANDLE;
    }
    world.reset();
    transmission.reset();
    extent = {};
    imagesInitialized = false;
    lastScene.clear();
    stats.accumulationCount = 0;
  }

  PathTracer::PathTracer() = default;

  PathTracer::~PathTracer() = default;

  void PathTracer::initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily) {
    m_gpu = std::make_unique<Gpu>();
    auto &g = *m_gpu;
    g.physical = physical;
    g.device = device;
    g.queueFamily = queueFamily;
    g.postCapabilities = denoise::capabilities(physical);
    vkGetPhysicalDeviceMemoryProperties(physical, &g.memory);
    vkGetPhysicalDeviceProperties(physical, &g.properties);
    VkPhysicalDeviceProperties2 properties = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                                              .pNext = &g.asProperties};
    vkGetPhysicalDeviceProperties2(physical, &properties);
#define LOAD(member, name)                                                                                   \
  g.member = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));                               \
  if (!g.member)                                                                                             \
  throw VK_ERROR_EXTENSION_NOT_PRESENT
    LOAD(createAs, vkCreateAccelerationStructureKHR);
    LOAD(destroyAs, vkDestroyAccelerationStructureKHR);
    LOAD(getAsAddress, vkGetAccelerationStructureDeviceAddressKHR);
    // Resolve 1.2 entry points only when tracing is selected, so loading the
    // raster runtime still works with a Vulkan 1.1 loader.
    LOAD(getBufferAddress, vkGetBufferDeviceAddress);
    LOAD(buildSizes, vkGetAccelerationStructureBuildSizesKHR);
    LOAD(buildAs, vkCmdBuildAccelerationStructuresKHR);
    LOAD(copyAs, vkCmdCopyAccelerationStructureKHR);
    LOAD(writeAsProperties, vkCmdWriteAccelerationStructuresPropertiesKHR);
#undef LOAD
    VkDescriptorSetLayoutBinding bindings[13] = {};
    for (unsigned i = 0; i < 13; ++i) {
      bindings[i] = {.binding = i,
                     .descriptorCount = i == 6 ? textureLimit : 1u,
                     .stageFlags =
                         VK_SHADER_STAGE_COMPUTE_BIT |
                         ((i == 2 || i == 3) ? VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT
                          : (i == 4 || i == 5 || i == 6) ? VK_SHADER_STAGE_FRAGMENT_BIT
                                                         : 0u)};
      bindings[i].descriptorType = i < 2    ? VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR
                                   : i == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                   : i == 3 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER
                                   : i != 6 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                            : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    VkDescriptorSetLayoutCreateInfo setLayout = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                                 .bindingCount = 13,
                                                 .pBindings = bindings};
    check(vkCreateDescriptorSetLayout(device, &setLayout, nullptr, &g.setLayout));
    VkDescriptorPoolSize poolSizes[] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 2},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
                                        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 8},
                                        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, textureLimit}};
    VkDescriptorPoolCreateInfo pool = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1,
                                       .poolSizeCount = 5,
                                       .pPoolSizes = poolSizes};
    check(vkCreateDescriptorPool(device, &pool, nullptr, &g.pool));
    VkDescriptorSetAllocateInfo allocate = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                            .descriptorPool = g.pool,
                                            .descriptorSetCount = 1,
                                            .pSetLayouts = &g.setLayout};
    check(vkAllocateDescriptorSets(device, &allocate, &g.set));
    VkPushConstantRange push = {VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8};
    VkPipelineLayoutCreateInfo layout = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                         .setLayoutCount = 1,
                                         .pSetLayouts = &g.setLayout,
                                         .pushConstantRangeCount = 1,
                                         .pPushConstantRanges = &push};
    check(vkCreatePipelineLayout(device, &layout, nullptr, &g.layout));
    const uint32_t *codes[] = {trace::compShader, trace::vertShader, trace::fragShader};
    size_t sizes[] = {sizeof(trace::compShader), sizeof(trace::vertShader), sizeof(trace::fragShader)};
    for (unsigned i = 0; i < 3; ++i) {
      VkShaderModuleCreateInfo info = {
          .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizes[i], .pCode = codes[i]};
      check(vkCreateShaderModule(device, &info, nullptr, &g.shaders[i]));
    }
    VkComputePipelineCreateInfo pipeline = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                  .module = g.shaders[0],
                  .pName = "main"},
        .layout = g.layout};
    check(vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &g.compute));
    uint32_t count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
    std::vector<VkQueueFamilyProperties> families(count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
    g.timestampBits = families.at(queueFamily).timestampValidBits;
    if (g.timestampBits && g.properties.limits.timestampComputeAndGraphics) {
      VkQueryPoolCreateInfo query = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = 4};
      check(vkCreateQueryPool(device, &query, nullptr, &g.timestamps));
    }
  }

  void PathTracer::configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput,
                                   VkFormat depthFormat, VkImageView depthView) {
    auto &g = *m_gpu;
    g.destroyTarget();
    g.extent = extent;
    g.encodeOutput = encodeOutput;
    g.world = g.image();
    g.transmission = g.image();
    g.sky = g.image(true);
    g.createPrimary(depthFormat, depthView);
    VkAttachmentDescription attachments[] = {
        {.format = VK_FORMAT_R32G32B32A32_SFLOAT,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_GENERAL},
        {.format = depthFormat,
         .samples = VK_SAMPLE_COUNT_1_BIT,
         .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
         .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
         .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
         .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL}};
    VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL},
        depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1,
                                 .pColorAttachments = &color,
                                 .pDepthStencilAttachment = &depthRef};
    VkSubpassDependency dependencies[] = {
        {.srcSubpass = VK_SUBPASS_EXTERNAL,
         .dstSubpass = 0,
         .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
         .dstStageMask =
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
         .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
         .dstAccessMask =
             VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT},
        {.srcSubpass = 0,
         .dstSubpass = VK_SUBPASS_EXTERNAL,
         .srcStageMask =
             VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
         .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
         .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT}};
    VkRenderPassCreateInfo rp{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                              .attachmentCount = 2,
                              .pAttachments = attachments,
                              .subpassCount = 1,
                              .pSubpasses = &subpass,
                              .dependencyCount = 2,
                              .pDependencies = dependencies};
    check(vkCreateRenderPass(g.device, &rp, nullptr, &g.skyPass));
    VkImageView views[] = {g.sky->view, depthView};
    VkFramebufferCreateInfo fb{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                               .renderPass = g.skyPass,
                               .attachmentCount = 2,
                               .pAttachments = views,
                               .width = extent.width,
                               .height = extent.height,
                               .layers = 1};
    check(vkCreateFramebuffer(g.device, &fb, nullptr, &g.skyFramebuffer));
    VkPipelineShaderStageCreateInfo stages[] = {{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_VERTEX_BIT,
                                                 .module = g.shaders[1],
                                                 .pName = "main"},
                                                {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
                                                 .module = g.shaders[2],
                                                 .pName = "main"}};
    VkPipelineVertexInputStateCreateInfo vertex = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineViewportStateCreateInfo viewport = {.sType =
                                                      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                  .viewportCount = 1,
                                                  .scissorCount = 1};
    VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1};
    VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    VkPipelineDepthStencilStateCreateInfo depth = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = VK_TRUE,
        .depthWriteEnable = VK_TRUE,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
        .maxDepthBounds = 1};
    VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic = {.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = 2,
                                                .pDynamicStates = states};
    for (unsigned i = 0; i < 2; ++i) {
      // First multiply the raster sky by colored transmission, then add diffuse radiance.
      // This keeps the visual sky out of lighting and supports tinted primary transmission.
      VkPipelineColorBlendAttachmentState attachment = {
          .blendEnable = VK_TRUE,
          .srcColorBlendFactor = i ? VK_BLEND_FACTOR_ONE : VK_BLEND_FACTOR_ZERO,
          .dstColorBlendFactor = i ? VK_BLEND_FACTOR_ZERO : VK_BLEND_FACTOR_SRC_COLOR,
          .colorBlendOp = VK_BLEND_OP_ADD,
          .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
          .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
          .alphaBlendOp = VK_BLEND_OP_ADD,
          .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT};
      VkPipelineColorBlendStateCreateInfo blending = {
          .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
          .attachmentCount = 1,
          .pAttachments = &attachment};
      depth.depthWriteEnable = !i;
      VkGraphicsPipelineCreateInfo pipeline = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                               .stageCount = 2,
                                               .pStages = stages,
                                               .pVertexInputState = &vertex,
                                               .pInputAssemblyState = &assembly,
                                               .pViewportState = &viewport,
                                               .pRasterizationState = &raster,
                                               .pMultisampleState = &samples,
                                               .pDepthStencilState = &depth,
                                               .pColorBlendState = &blending,
                                               .pDynamicState = &dynamic,
                                               .layout = g.layout,
                                               .renderPass = pass};
      check(vkCreateGraphicsPipelines(g.device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &g.composite[i]));
    }
  }

  void PathTracer::destroyTarget() {
    if (m_gpu)
      m_gpu->destroyTarget();
  }

  remixapi_TracingStatisticsEXT PathTracer::statistics() const { return m_gpu->stats; }

} // namespace remix
