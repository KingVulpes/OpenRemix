#include "raster/gpu.h"

namespace remix {
  using namespace scene;
  using namespace raster::detail;
  namespace {
    uint64_t pipelineKey(const Draw &draw) {
      const auto &b = draw.blend;
      return uint64_t(b.src) | uint64_t(b.dst) << 4 | uint64_t(b.op) << 8 | uint64_t(b.srcAlpha) << 11 |
             uint64_t(b.dstAlpha) << 15 | uint64_t(b.alphaOp) << 19 | uint64_t(b.mask) << 22 |
             uint64_t(b.enabled) << 26 | uint64_t(draw.doubleSided) << 27 | uint64_t(draw.depth) << 28 |
             uint64_t(draw.depth && (draw.depthWriteOverride ? draw.depthWrite : (!b.enabled || draw.ui)))
                 << 29 |
             uint64_t(draw.depthCompare) << 30 | uint64_t(draw.depthBias) << 33;
    }
  } // namespace

  VkPipeline RasterRenderer::Gpu::pipeline(const Draw &draw) {
    const bool sky = skyPass && draw.camera == REMIXAPI_CAMERA_TYPE_SKY;
    const uint64_t key = pipelineKey(draw) | uint64_t(sky) << 34;
    if (auto it = pipelines.find(key); it != pipelines.end()) {
      return it->second;
    }
    if (pipelines.size() >= 256) {
      throw VK_ERROR_TOO_MANY_OBJECTS;
    }
    const VkPipelineShaderStageCreateInfo stages[] = {
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_VERTEX_BIT,
         .module = vertex,
         .pName = "main"},
        {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
         .module = fragment,
         .pName = "main"}};
    const VkVertexInputBindingDescription binding = {0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    const VkVertexInputAttributeDescription attributes[] = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, position)},
        {1, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, uv)},
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(Vertex, color)}};
    const VkPipelineVertexInputStateCreateInfo input = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1,
        .pVertexBindingDescriptions = &binding,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = attributes};
    const VkPipelineInputAssemblyStateCreateInfo assembly = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    const VkPipelineViewportStateCreateInfo viewport = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1};
    const VkPipelineRasterizationStateCreateInfo raster = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VkCullModeFlags(draw.doubleSided ? VK_CULL_MODE_NONE : VK_CULL_MODE_BACK_BIT),
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = draw.depthBias,
        .depthBiasConstantFactor = -1.f,
        .depthBiasSlopeFactor = -1.f,
        .lineWidth = 1};
    const VkPipelineMultisampleStateCreateInfo samples = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
    const VkPipelineDepthStencilStateCreateInfo depthState = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = draw.depth,
        .depthWriteEnable =
            draw.depth && (draw.depthWriteOverride ? draw.depthWrite : (!draw.blend.enabled || draw.ui)),
        .depthCompareOp = VkCompareOp(draw.depthCompare)};
    const auto &b = draw.blend;
    const VkPipelineColorBlendAttachmentState blend = {.blendEnable = b.enabled,
                                                       .srcColorBlendFactor = VkBlendFactor(b.src),
                                                       .dstColorBlendFactor = VkBlendFactor(b.dst),
                                                       .colorBlendOp = VkBlendOp(b.op),
                                                       .srcAlphaBlendFactor = VkBlendFactor(b.srcAlpha),
                                                       .dstAlphaBlendFactor = VkBlendFactor(b.dstAlpha),
                                                       .alphaBlendOp = VkBlendOp(b.alphaOp),
                                                       .colorWriteMask = b.mask};
    const VkPipelineColorBlendStateCreateInfo blending = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &blend};
    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                                            VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    const VkPipelineDynamicStateCreateInfo dynamic = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3,
        .pDynamicStates = dynamicStates};
    const VkGraphicsPipelineCreateInfo info = {.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                               .stageCount = 2,
                                               .pStages = stages,
                                               .pVertexInputState = &input,
                                               .pInputAssemblyState = &assembly,
                                               .pViewportState = &viewport,
                                               .pRasterizationState = &raster,
                                               .pMultisampleState = &samples,
                                               .pDepthStencilState = &depthState,
                                               .pColorBlendState = &blending,
                                               .pDynamicState = &dynamic,
                                               .layout = layout,
                                               .renderPass = sky ? skyPass : pass};
    VkPipeline result = VK_NULL_HANDLE;
    const auto status = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info, nullptr, &result);
    if (status != VK_SUCCESS) {
      if (result) {
        vkDestroyPipeline(device, result, nullptr);
      }
      check(status);
    }
    try {
      pipelines.emplace(key, result);
    } catch (...) {
      vkDestroyPipeline(device, result, nullptr);
      throw;
    }
    return result;
  }

} // namespace remix
