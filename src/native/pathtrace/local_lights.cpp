#include "pathtrace/gpu.h"

namespace remix {
using namespace trace::detail;

// A uniform runtime branch still reserves registers for the many-light sampler.
// Specializing it away restores occupancy for water and ordinary sky/sun
// scenes. Variants are lazy, cached, and share the source pipeline's
// fence/layout lifetime.
VkPipeline PathTracer::Gpu::selectLightingPipeline(VkPipeline pipeline) {
  auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return pipeline;
  auto &source = found->second;
  const bool small = localIlluminationActive && lightBvh.nodes().size() >= 3 && lightBvh.nodes().size() <= 7;
  const bool guided = localGuidingActive && source.supportsGuiding;
  const bool resampling = localResamplingActive && source.supportsGuiding;
  const bool split = splitLightingActive && source.supportsGuiding;
  if (localIlluminationActive && !guided && !small && !resampling && !split)
    return pipeline;
  auto &variant = split ? source.withSplit[guided] : resampling ? source.withResampling[guided] : small ? source.small[guided] : localIlluminationActive ? source.withGuiding : source.withoutLocal;
  auto &attempted = split ? source.splitAttempted[guided] : resampling ? source.resamplingAttempted[guided] : small ? source.smallAttempted[guided] : localIlluminationActive ? source.guidingAttempted : source.attempted;
  if (!attempted) {
    attempted = true;
    VkShaderModuleCreateInfo ci{.sType =
                                    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                .codeSize = source.bytes,
                                .pCode = source.code};
    VkShaderModule module{};
    auto result = vkCreateShaderModule(device, &ci, nullptr, &module);
    if (result == VK_SUCCESS) {
      const VkBool32 enabled[] = {VkBool32(localIlluminationActive), VkBool32(guided), VkBool32(small), VkBool32(resampling), VkBool32(split)};
      const VkSpecializationMapEntry entries[] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)},
                                                 {2, 2 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {3, 3 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {4, 4 * sizeof(VkBool32), sizeof(VkBool32)}};
      const VkSpecializationInfo specialization{5, entries, sizeof(enabled), enabled};
      const VkComputePipelineCreateInfo create{
          .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = module,
                    .pName = "main",
                    .pSpecializationInfo = &specialization},
          .layout = source.layout};
      result = vkCreateComputePipelines(device, pipelineCache, 1, &create,
                                        nullptr, &variant);
      vkDestroyShaderModule(device, module, nullptr);
    }
    // This specialization only improves performance. The ordinary pipeline
    // retains ordinary LightBVH sampling if guidance cannot be allocated,
    // or a black local signal when illumination is disabled.
    if (result == VK_ERROR_DEVICE_LOST)
      throw result;
    if (result != VK_SUCCESS && guided) {
      lightGuiding.pipelineFailed = true;
      lightGuiding.valid = false;
      lightGuiding.stats.active = 0;
      localGuidingActive = false;
      std::strncpy(lightGuiding.stats.unavailableReason,
                   "Guided shader initialization failed; using LightBVH sampling",
                   sizeof(lightGuiding.stats.unavailableReason) - 1);
    }
  }
  return variant ? variant : pipeline;
}

void PathTracer::Gpu::forgetLightingPipeline(VkPipeline pipeline) {
  const auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return;
  if (found->second.withoutLocal)
    vkDestroyPipeline(device, found->second.withoutLocal, nullptr);
  if (found->second.withGuiding)
    vkDestroyPipeline(device, found->second.withGuiding, nullptr);
  for (auto variant : found->second.withResampling)
    if (variant)
      vkDestroyPipeline(device, variant, nullptr);
  for (auto variant : found->second.withSplit)
    if (variant)
      vkDestroyPipeline(device, variant, nullptr);
  for (auto variant : found->second.small)
    if (variant)
      vkDestroyPipeline(device, variant, nullptr);
  lightingPipelines.erase(found);
}
} // namespace remix
