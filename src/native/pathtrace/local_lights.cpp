#include "pathtrace/gpu.h"
#include "runtime/pipelines.h"

namespace remix {
using namespace trace::detail;

// A uniform runtime branch still reserves registers for the many-light sampler.
// Specializing it away restores occupancy for water and ordinary sky/sun
// scenes. Variants are lazy, cached, and share the source pipeline's
// fence/layout lifetime.
VkPipeline PathTracer::Gpu::selectLightingPipeline(VkPipeline pipeline, bool combinedDirect, bool separateIndirect, uint32_t sparse) {
  selectedLightingKey = UINT32_MAX;
  auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return separateIndirect || sparse ? VK_NULL_HANDLE : pipeline;
  auto &source = found->second;
  const bool smallSet = localIlluminationActive && lightBvh.nodes().size() >= 3 && lightBvh.nodes().size() <= 7;
  const bool guided = localGuidingActive && source.supportsGuiding;
  const bool resampling = localResamplingActive && source.supportsGuiding;
  const bool split = splitLightingActive && source.supportsGuiding && !combinedDirect;
  const bool angular = localIlluminationActive && localAngularActive && source.supportsGuiding;
  // Combined mode also uses the split opaque pipeline in raw rendering. Its
  // RIS/small-set switches must be part of the cache key when budgets change.
  const bool firstHit = localFirstHitActive && source.supportsGuiding && source.preferFirstHit;
  const bool reuse = pbrReuse.active;
  const bool roulette = rouletteActive && source.supportsRoulette;
  // rtx.native.pathtrace.shadowEarlyOut; off keeps the existing variants.
  const bool earlyOut = shadowEarlyOutActive;
  const bool keyed = separateIndirect || angular || combinedDirect || split || reuse || rayProfile.path || firstHit || roulette || !decalMaterialsActive || earlyOut || sparse;
  const uint32_t key = uint32_t(guided) | uint32_t(smallSet) << 1 | uint32_t(resampling) << 2 |
                       uint32_t(split) << 3 | uint32_t(combinedDirect) << 4 | uint32_t(angular) << 5 |
                       uint32_t(localIlluminationActive) << 6 | uint32_t(reuse) << 7 | uint32_t(rayProfile.path != nullptr) << 8 | uint32_t(firstHit) << 9 | uint32_t(separateIndirect) << 10 | uint32_t(roulette) << 11 | uint32_t(decalMaterialsActive) << 12 |
                       uint32_t(earlyOut) << 13 | sparse << 14;
  selectedLightingKey = key;
  if (localIlluminationActive && !guided && !smallSet && !resampling && !split && !angular && !combinedDirect && !reuse && !rayProfile.path && !firstHit && !separateIndirect && !roulette && decalMaterialsActive && !earlyOut && !sparse)
    return pipeline;
  auto &variant = keyed ? source.specialized[key].first : resampling ? source.withResampling[guided] : smallSet ? source.smallSet[guided] : localIlluminationActive ? source.withGuiding : source.withoutLocal;
  auto &attempted = keyed ? source.specialized[key].second : resampling ? source.resamplingAttempted[guided] : smallSet ? source.smallAttempted[guided] : localIlluminationActive ? source.guidingAttempted : source.attempted;
  if (!attempted) {
    attempted = true;
    VkShaderModuleCreateInfo ci{.sType =
                                    VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                .codeSize = source.bytes,
                                .pCode = source.code};
    VkShaderModule module{};
    auto result = pipeline::createShaderModule(device, &ci, nullptr, &module);
    if (result == VK_SUCCESS) {
      const VkBool32 enabled[] = {VkBool32(localIlluminationActive), VkBool32(guided), VkBool32(smallSet), VkBool32(resampling), VkBool32(split), VkBool32(angular), VkBool32(reuse), VkBool32(rayProfile.path != nullptr), VkBool32(firstHit), VkBool32(separateIndirect), VkBool32(roulette), VkBool32(decalMaterialsActive), VkBool32(earlyOut), sparse};
      const VkSpecializationMapEntry entries[] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)},
                                                 {2, 2 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {3, 3 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {4, 4 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {5, 5 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {6, 6 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {7, 7 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {8, 8 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {9, 9 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {10, 10 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {11, 11 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {12, 12 * sizeof(VkBool32), sizeof(VkBool32)},
                                                 {13, 13 * sizeof(VkBool32), sizeof(uint32_t)}};
      const VkSpecializationInfo specialization{14, entries, sizeof(enabled), enabled};
      const VkComputePipelineCreateInfo create{
          .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
          .stage = {.sType =
                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                    .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                    .module = module,
                    .pName = "main",
                    .pSpecializationInfo = &specialization},
          .layout = source.layout};
      result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1,
                                                &create, nullptr, &variant);
      pipeline::destroyShaderModule(device, module, nullptr);
    }
    if (result == VK_NOT_READY) {
      attempted = false;
      if (reuse)
        pbrReuse.valid = pbrReuse.active = false;
      return separateIndirect || sparse ? VK_NULL_HANDLE : pipeline;
    }
    // This specialization only improves performance. The ordinary pipeline
    // retains ordinary LightBVH sampling if guidance cannot be allocated,
    // or a black local signal when illumination is disabled.
    if (result == VK_ERROR_DEVICE_LOST)
      throw result;
    if (result != VK_SUCCESS && roulette && !sparse) {
      rouletteFailed = true;
      rouletteActive = false;
      std::fprintf(stderr, "Adaptive termination unavailable; using fixed-limit tracing\n");
    }
    if (result != VK_SUCCESS && guided && !separateIndirect && !sparse) {
      lightGuiding.pipelineFailed = true;
      lightGuiding.valid = false;
      lightGuiding.stats.active = 0;
      localGuidingActive = false;
      std::strncpy(lightGuiding.stats.unavailableReason,
                   "Guided shader initialization failed; using LightBVH sampling",
                   sizeof(lightGuiding.stats.unavailableReason) - 1);
    }
  }
  if (reuse && !variant) pbrReuse.valid = pbrReuse.active = false;
  // A split consumer must never silently execute the inline fallback after
  // its producer has run (duplicate tracing and cache/profile side effects).
  if ((separateIndirect || sparse) && !variant) return VK_NULL_HANDLE;
  return variant ? variant : pipeline;
}

void PathTracer::Gpu::forgetLightingPipeline(VkPipeline pipeline) {
  const auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return;
  if (found->second.withoutLocal)
    pipeline::destroyPipeline(device, found->second.withoutLocal, nullptr);
  if (found->second.withGuiding)
    pipeline::destroyPipeline(device, found->second.withGuiding, nullptr);
  for (auto variant : found->second.withResampling)
    if (variant)
      pipeline::destroyPipeline(device, variant, nullptr);
  for (const auto &[key, variant] : found->second.specialized)
    if (variant.first)
      pipeline::destroyPipeline(device, variant.first, nullptr);
  for (auto variant : found->second.smallSet)
    if (variant)
      pipeline::destroyPipeline(device, variant, nullptr);
  lightingPipelines.erase(found);
}
} // namespace remix
