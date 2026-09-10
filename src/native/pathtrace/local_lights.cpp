#include "pathtrace/gpu.h"

namespace remix {
using namespace trace::detail;

// A uniform runtime branch still reserves registers for the many-light sampler.
// Specializing it away restores occupancy for water and ordinary sky/sun
// scenes. Variants are lazy, cached, and share the source pipeline's
// fence/layout lifetime.
VkPipeline PathTracer::Gpu::selectLightingPipeline(VkPipeline pipeline) {
  if (localIlluminationActive)
    return pipeline;
  auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return pipeline;
  auto &source = found->second;
  if (!source.attempted) {
    source.attempted = true;
    VkShaderModuleCreateInfo ci{.sType =
                                    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                .codeSize = source.bytes,
                                .pCode = source.code};
    VkShaderModule module{};
    auto result = vkCreateShaderModule(device, &ci, nullptr, &module);
    if (result == VK_SUCCESS) {
      const VkBool32 enabled = VK_FALSE;
      const VkSpecializationMapEntry entry{0, 0, sizeof(enabled)};
      const VkSpecializationInfo specialization{1, &entry, sizeof(enabled),
                                                &enabled};
      const VkComputePipelineCreateInfo create{
          .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = module,
                    .pName = "main",
                    .pSpecializationInfo = &specialization},
          .layout = source.layout};
      result = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &create,
                                        nullptr, &source.withoutLocal);
      vkDestroyShaderModule(device, module, nullptr);
    }
    // This specialization only improves performance. The ordinary pipeline
    // produces the same black local signal if a variant cannot be allocated.
    if (result == VK_ERROR_DEVICE_LOST)
      throw result;
  }
  return source.withoutLocal ? source.withoutLocal : pipeline;
}

void PathTracer::Gpu::forgetLightingPipeline(VkPipeline pipeline) {
  const auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return;
  if (found->second.withoutLocal)
    vkDestroyPipeline(device, found->second.withoutLocal, nullptr);
  lightingPipelines.erase(found);
}
} // namespace remix
