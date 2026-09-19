#include "pathtrace/gpu.h"
#include "pathtrace/shaders/embedded.h"

namespace remix {
  using namespace trace::detail;

  void PathTracer::Gpu::destroyPrimary() {
    for (unsigned i = 0; i < 2; ++i) {
      if (primaryPipeline[i])
        vkDestroyPipeline(device, primaryPipeline[i], nullptr);
      if (primaryFramebuffer[i])
        vkDestroyFramebuffer(device, primaryFramebuffer[i], nullptr);
      if (primaryPass[i])
        vkDestroyRenderPass(device, primaryPass[i], nullptr);
      primaryPipeline[i] = VK_NULL_HANDLE;
      primaryFramebuffer[i] = VK_NULL_HANDLE;
      primaryPass[i] = VK_NULL_HANDLE;
    }
    if (primaryLayout)
      vkDestroyPipelineLayout(device, primaryLayout, nullptr);
    primaryLayout = VK_NULL_HANDLE;
    for (auto &target : primary)
      target.reset();
    primaryDraws.clear();
    primaryInitialized = false;
  }

  void PathTracer::Gpu::createPrimary(VkFormat depthFormat, VkImageView depthView) {
    // Position and previous position retain float32 precision. Normal/flags and
    // linear albedo use smaller attachments to limit G-buffer bandwidth.
    const VkFormat formats[] = {VK_FORMAT_R32G32B32A32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT,
                                VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R32G32B32A32_SFLOAT,
                                VK_FORMAT_R32_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT};
    for (unsigned i = 0; i < 7; ++i)
      primary[i] = image(true, formats[i], i == 4 ? displayExtent : extent);
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                             sizeof(PrimaryPush)};
    VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                  .setLayoutCount = 1,
                                  .pSetLayouts = &setLayout,
                                  .pushConstantRangeCount = 1,
                                  .pPushConstantRanges = &push};
    check(vkCreatePipelineLayout(device, &li, nullptr, &primaryLayout));

    for (unsigned pass = 0; pass < 2; ++pass) {
      const VkExtent2D size = pass ? displayExtent : extent;
      const unsigned colors = pass ? 1 : 6;
      VkAttachmentDescription attachments[7] = {};
      VkAttachmentReference references[6] = {};
      VkImageView views[7] = {};
      for (unsigned i = 0; i < colors; ++i) {
        const unsigned target = pass ? 4 : i >= 4 ? i + 1 : i;
        attachments[i] = {.format = formats[target],
                          .samples = VK_SAMPLE_COUNT_1_BIT,
                          .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                          .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                          .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                          .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                          .finalLayout = VK_IMAGE_LAYOUT_GENERAL};
        references[i] = {i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        views[i] = primary[target]->view;
      }
      attachments[colors] = {.format = depthFormat,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
                             .storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                             .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                             .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                             .finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      views[colors] = depthView;
      VkAttachmentReference depth{colors, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
      VkSubpassDescription subpass{.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                   .colorAttachmentCount = colors,
                                   .pColorAttachments = references,
                                   .pDepthStencilAttachment = &depth};
      const auto attachmentStages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
      VkSubpassDependency dependencies[] = {
          {.srcSubpass = VK_SUBPASS_EXTERNAL,
           .dstSubpass = 0,
           .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
           .dstStageMask = attachmentStages,
           .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
           .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT},
          {.srcSubpass = 0,
           .dstSubpass = VK_SUBPASS_EXTERNAL,
           .srcStageMask = attachmentStages,
           .dstStageMask = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | attachmentStages,
           .srcAccessMask =
               VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
           .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                            VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT}};
      VkRenderPassCreateInfo rp{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
                                .attachmentCount = colors + 1,
                                .pAttachments = attachments,
                                .subpassCount = 1,
                                .pSubpasses = &subpass,
                                .dependencyCount = 2,
                                .pDependencies = dependencies};
      check(vkCreateRenderPass(device, &rp, nullptr, &primaryPass[pass]));
      VkFramebufferCreateInfo fb{.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
                                 .renderPass = primaryPass[pass],
                                 .attachmentCount = colors + 1,
                                 .pAttachments = views,
                                 .width = size.width,
                                 .height = size.height,
                                 .layers = 1};
      check(vkCreateFramebuffer(device, &fb, nullptr, &primaryFramebuffer[pass]));

      VkShaderModule modules[2] = {};
      const uint32_t *code[] = {trace::gbufferVertShader,
                                pass ? trace::depthFragShader : trace::gbufferFragShader};
      const size_t sizes[] = {sizeof(trace::gbufferVertShader),
                              pass ? sizeof(trace::depthFragShader) : sizeof(trace::gbufferFragShader)};
      try {
        VkPipelineShaderStageCreateInfo stages[2] = {};
        for (unsigned i = 0; i < 2; ++i) {
          VkShaderModuleCreateInfo info{
              .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizes[i], .pCode = code[i]};
          check(vkCreateShaderModule(device, &info, nullptr, &modules[i]));
          stages[i] = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                       .stage = i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT,
                       .module = modules[i],
                       .pName = "main"};
        }
        VkPipelineVertexInputStateCreateInfo vertex{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
        VkPipelineViewportStateCreateInfo viewport{.sType =
                                                       VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                   .viewportCount = 1,
                                                   .scissorCount = 1};
        VkPipelineRasterizationStateCreateInfo raster{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .lineWidth = 1};
        VkPipelineMultisampleStateCreateInfo samples{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT};
        VkPipelineDepthStencilStateCreateInfo depthState{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = VK_TRUE,
            .depthWriteEnable = VK_TRUE,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
            .maxDepthBounds = 1};
        VkPipelineColorBlendAttachmentState blend[6] = {};
        for (unsigned i = 0; i < colors; ++i)
          blend[i].colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blending{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = colors,
            .pAttachments = blend};
        VkDynamicState states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{.sType =
                                                     VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                 .dynamicStateCount = 2,
                                                 .pDynamicStates = states};
        VkGraphicsPipelineCreateInfo pipeline{.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                              .stageCount = 2,
                                              .pStages = stages,
                                              .pVertexInputState = &vertex,
                                              .pInputAssemblyState = &assembly,
                                              .pViewportState = &viewport,
                                              .pRasterizationState = &raster,
                                              .pMultisampleState = &samples,
                                              .pDepthStencilState = &depthState,
                                              .pColorBlendState = &blending,
                                              .pDynamicState = &dynamic,
                                              .layout = primaryLayout,
                                              .renderPass = primaryPass[pass]};
        check(
            vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, &primaryPipeline[pass]));
      } catch (...) {
        for (auto module : modules)
          if (module)
            vkDestroyShaderModule(device, module, nullptr);
        throw;
      }
      for (auto module : modules)
        vkDestroyShaderModule(device, module, nullptr);
    }
  }

  void PathTracer::Gpu::recordPrimary(VkCommandBuffer command, bool rasterPrimary) {
    if (!rasterPrimary) {
      // The reference shader statically declares these descriptors even when it
      // does not read them. Keep their descriptor layouts valid from first use.
      if (!primaryInitialized) {
        VkImageMemoryBarrier transitions[7] = {};
        for (unsigned i = 0; i < 7; ++i)
          transitions[i] = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                            .image = primary[i]->handle,
                            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 7, transitions);
        primaryInitialized = true;
      }
      if (!upscaler) return;
    }
    for (unsigned pass = rasterPrimary ? 0 : 1; pass < 2; ++pass) {
      const VkExtent2D size = pass ? displayExtent : extent;
      const unsigned colors = pass ? 1 : 6;
      VkClearValue clear[7] = {};
      if (pass)
        clear[0].color.float32[0] = 1;
      clear[colors].depthStencil.depth = 1;
      VkRenderPassBeginInfo begin{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
                                  .renderPass = primaryPass[pass],
                                  .framebuffer = primaryFramebuffer[pass],
                                  .renderArea = {{0, 0}, size},
                                  .clearValueCount = colors + 1,
                                  .pClearValues = clear};
      vkCmdBeginRenderPass(command, &begin, VK_SUBPASS_CONTENTS_INLINE);
      VkViewport viewport{0, 0, float(size.width), float(size.height), 0, 1};
      VkRect2D scissor{{0, 0}, size};
      vkCmdSetViewport(command, 0, 1, &viewport);
      vkCmdSetScissor(command, 0, 1, &scissor);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, primaryPipeline[pass]);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, primaryLayout, 0, 1, &set, 0,
                              nullptr);
      for (const auto &draw : primaryDraws) {
        if (pass && draw.blended)
          continue;
        PrimaryPush push{draw.model,
                         {pass ? 0.f : temporalFrame.jitter[0], pass ? 0.f : temporalFrame.jitter[1]},
                         draw.surfaceIndex,
                         pass};
        vkCmdPushConstants(command, primaryLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(push), &push);
        vkCmdDraw(command, draw.count, 1, 0, 0);
      }
      vkCmdEndRenderPass(command);
    }
    primaryInitialized = true;
  }
} // namespace remix
