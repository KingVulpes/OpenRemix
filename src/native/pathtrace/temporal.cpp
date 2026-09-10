#include "pathtrace/gpu.h"
#include "pathtrace/shaders/embedded.h"

namespace remix {
  using namespace trace::detail;
  void PathTracer::Gpu::destroyPost() {
    if (denoiser) {
      auto s = denoiser->statistics();
      historyResets += s.historyResets;
      denoisedFrames += s.renderedFrames;
      postPeakMemory = std::max(postPeakMemory, s.peakMemoryBytes);
    }
    destroySharc(true);
    if (temporalTransparencyCompute)
      vkDestroyPipeline(device, temporalTransparencyCompute, nullptr);
    temporalTransparencyCompute = VK_NULL_HANDLE;
    if (temporalCompute)
      vkDestroyPipeline(device, temporalCompute, nullptr);
    if (confidenceCompute)
      vkDestroyPipeline(device, confidenceCompute, nullptr);
    if (temporalLayout)
      vkDestroyPipelineLayout(device, temporalLayout, nullptr);
    temporalCompute = confidenceCompute = VK_NULL_HANDLE;
    temporalLayout = VK_NULL_HANDLE;
    denoiser.reset();
    postAttempted = false;
    postFailure.clear();
  }
  void PathTracer::Gpu::beginFrame(const TraceOptions &options) {
    retiredPreviousVertices.clear(); // beginFrame runs after the previous submission fence.
    const auto &caps = postCapabilities;
    const bool reblur = options.denoiser && !options.accumulate && !options.sharcDebug && caps.reblurShSupported;
    const bool taa = options.taa && !options.accumulate && !options.sharcDebug && caps.taaSupported;
    if (reblur != postReblur || taa != postTaa) {
      destroyPost();
      postReblur = reblur;
      postTaa = taa;
      history.clear();
    }
    if (!postAttempted)
      postFailure.clear();
    if ((options.denoiser && !caps.reblurShSupported) || (options.taa && !caps.taaSupported))
      postFailure = caps.unavailableReason;
    if ((!reblur && !taa) || postAttempted)
      return;
    postAttempted = true;
    try {
      auto next = std::make_unique<denoise::Renderer>();
      next->initialize(physical, device, extent, reblur, queueFamily);
      VkDescriptorSetLayout layouts[] = {setLayout, next->inputLayout()};
      VkPipelineLayoutCreateInfo li{.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                    .setLayoutCount = 2,
                                    .pSetLayouts = layouts};
      check(vkCreatePipelineLayout(device, &li, nullptr, &temporalLayout));
      auto make = [&](const uint32_t *code, size_t size, VkPipeline &out) {
        VkShaderModuleCreateInfo ci{
            .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code};
        VkShaderModule shader{};
        check(vkCreateShaderModule(device, &ci, nullptr, &shader));
        VkComputePipelineCreateInfo pi{.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                 .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                 .module = shader,
                                                 .pName = "main"},
                                       .layout = temporalLayout};
        auto result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pi, nullptr, &out);
        vkDestroyShaderModule(device, shader, nullptr);
        check(result);
      };
      make(trace::temporalShader, sizeof(trace::temporalShader), temporalCompute);
      make(trace::temporalTransparencyShader, sizeof(trace::temporalTransparencyShader),
           temporalTransparencyCompute);
      if (reblur)
        make(trace::confidenceShader, sizeof(trace::confidenceShader), confidenceCompute);
      denoiser = std::move(next);
    } catch (VkResult r) {
      if (r == VK_ERROR_DEVICE_LOST)
        throw;
      destroyPost();
      postAttempted = true;
      postFailure = "Temporal resource initialization failed (Vulkan " + std::to_string(r) +
                    "); raw tracing remains active";
    } catch (const std::exception &e) {
      destroyPost();
      postAttempted = true;
      postFailure = std::string("Temporal initialization failed: ") + e.what();
    }
  }
  void PathTracer::beginFrame(const TraceOptions &options) {
    m_gpu->beginFrame(options);
    m_gpu->beginSharc(options);
  }
  VkRenderPass PathTracer::skyPass() const { return m_gpu->skyPass; }
  void PathTracer::submitted() {
    auto &g = *m_gpu;
    if (!g.rendered)
      return;
    g.rendered = false;
    g.history.commit();
    if (g.sharc.stats.active) {
      ++g.sharc.stats.renderedFrames;
      ++g.sharc.warmFrames;
    }
    g.retiredPreviousVertices = std::move(g.previousVertices);
    g.previousVertices = std::move(g.pendingVertices);
    g.previousCamera = scene::inverse(g.temporalFrame.inverseCamera);
    g.previousView = g.temporalFrame.view;
    g.previousProjection = g.pendingProjection;
    std::copy_n(g.temporalFrame.jitter, 2, g.previousJitter);
    g.previousSeed = uint32_t(g.sequence - 1);
    g.previousTime = g.pendingTime;
    if (g.denoiser)
      g.denoiser->submitted();
  }
  remixapi_DenoisingStatisticsEXT PathTracer::denoisingStatistics() const {
    auto &g = *m_gpu;
    auto result =
        g.denoiser ? g.denoiser->statistics()
                   : remixapi_DenoisingStatisticsEXT{.structSize = sizeof(remixapi_DenoisingStatisticsEXT)};
    result.historyResets += g.historyResets;
    result.renderedFrames += g.denoisedFrames;
    result.peakMemoryBytes = std::max(result.peakMemoryBytes, g.postPeakMemory);
    std::strncpy(result.unavailableReason, g.postFailure.c_str(), sizeof(result.unavailableReason) - 1);
    return result;
  }
} // namespace remix
