#include "pathtrace/gpu.h"
#include "runtime/pipelines.h"
#include <algorithm>

namespace remix {
using namespace trace::detail;

// Variants specialize the light sampler's structure (local lights, guiding,
// the small set, resampling, split lighting), a dispatch's role (separate
// indirect pass, sparse launches, ray profile) and the shadow early-out
// setting: each is a 20-45 s compile, so only those stay variants, and the
// scene-selected ones are prewarmed (prewarmLightingVariants). The remaining
// switches travel as push constants (bindTrace); they trim code, not
// registers. Variants are lazy, cached, and share the source pipeline's
// fence/layout lifetime.
VkPipeline PathTracer::Gpu::selectLightingPipeline(VkPipeline pipeline, bool combinedDirect, bool separateIndirect, uint32_t sparse) {
  selectedLightingKey = UINT32_MAX;
  selectedFeatures = defaultFeatures;
  selectedLayout = layout;
  auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return separateIndirect || sparse ? VK_NULL_HANDLE : pipeline;
  auto &source = found->second;
  selectedLayout = source.layout;
  // The small set's sampler costs a transparency pass 255 registers: only
  // the opaque (guiding-capable) passes take it.
  const bool smallSet = localIlluminationActive && source.supportsGuiding && lightBvh.nodes().size() >= 3 && lightBvh.nodes().size() <= 7;
  const bool guided = localGuidingActive && source.supportsGuiding;
  const bool resampling = localResamplingActive && source.supportsGuiding;
  const bool split = splitLightingActive && source.supportsGuiding && !combinedDirect;
  const bool angular = localIlluminationActive && localAngularActive && source.supportsGuiding;
  const bool firstHit = localFirstHitActive && source.supportsGuiding && source.preferFirstHit;
  const bool roulette = rouletteActive && source.supportsRoulette;
  // rtx.native.pathtrace.shadowEarlyOut; off keeps the existing variants.
  const bool earlyOut = shadowEarlyOutActive;
  selectedFeatures = (decalMaterialsActive ? featureDecals : 0u) | (roulette ? featureRoulette : 0u) |
                     (pbrReuse.active ? featurePbrReuse : 0u) | (angular ? featureAngularEmitters : 0u) |
                     (firstHit ? featureFirstOpaqueHit : 0u);
  // Combined mode also uses the split opaque pipeline in raw rendering. Its
  // RIS/small-set switches must be part of the cache key when budgets change.
  const uint32_t key = (guided ? variantGuided : 0u) | (smallSet ? variantSmallSet : 0u) |
                       (resampling ? variantResampling : 0u) | (split ? variantSplit : 0u) |
                       (combinedDirect ? variantCombinedDirect : 0u) | (localIlluminationActive ? variantLocal : 0u) |
                       (rayProfile.path ? variantProfile : 0u) | (separateIndirect ? variantSeparateIndirect : 0u) |
                       (earlyOut ? variantEarlyOut : 0u) | sparse << variantSparseShift;
  selectedLightingKey = key;
  // The source itself: local lights, ordinary sampling, its own early-out
  // and indirect role.
  if (key == (variantLocal | (source.baseEarlyOut ? variantEarlyOut : 0u) |
              (source.baseSeparateIndirect ? variantSeparateIndirect : 0u)))
    return pipeline;
  auto &[variant, attempted] = source.specialized[key];
  if (!attempted) {
    attempted = true;
    const auto result = requestLightingVariant(source, key, &variant, false);
    if (result == VK_NOT_READY) {
      attempted = false;
      // A consumer source stands in for its own variants (plain sampling);
      // a producer-dependent role without one has no fallback.
      return sparse || (separateIndirect && !source.baseSeparateIndirect) ? VK_NULL_HANDLE : pipeline;
    }
    // This specialization only improves performance. The ordinary pipeline
    // retains ordinary LightBVH sampling if guidance cannot be allocated,
    // or a black local signal when illumination is disabled.
    if (result == VK_ERROR_DEVICE_LOST)
      throw result;
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
  // A split consumer must never silently execute the inline fallback after
  // its producer has run (duplicate tracing and cache/profile side effects).
  if ((sparse || (separateIndirect && !source.baseSeparateIndirect)) && !variant) return VK_NULL_HANDLE;
  return variant ? variant : pipeline;
}

VkResult PathTracer::Gpu::requestLightingVariant(LightingPipeline &source, uint32_t key, VkPipeline *out,
                                                 bool prefetch) {
  VkShaderModuleCreateInfo ci{.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                              .codeSize = source.bytes,
                              .pCode = source.code};
  VkShaderModule module{};
  auto result = pipeline::createShaderModule(device, &ci, nullptr, &module);
  if (result != VK_SUCCESS)
    return result;
  const auto bit = [key](uint32_t mask) { return VkBool32((key & mask) != 0); };
  const VkBool32 enabled[] = {bit(variantLocal),   bit(variantGuided),  bit(variantSmallSet),
                              bit(variantResampling), bit(variantSplit), bit(variantProfile),
                              bit(variantSeparateIndirect), bit(variantEarlyOut), key >> variantSparseShift};
  const VkSpecializationMapEntry entries[] = {{0, 0, sizeof(VkBool32)}, {1, sizeof(VkBool32), sizeof(VkBool32)},
                                             {2, 2 * sizeof(VkBool32), sizeof(VkBool32)},
                                             {3, 3 * sizeof(VkBool32), sizeof(VkBool32)},
                                             {4, 4 * sizeof(VkBool32), sizeof(VkBool32)},
                                             {7, 5 * sizeof(VkBool32), sizeof(VkBool32)},
                                             {9, 6 * sizeof(VkBool32), sizeof(VkBool32)},
                                             {12, 7 * sizeof(VkBool32), sizeof(VkBool32)},
                                             {13, 8 * sizeof(VkBool32), sizeof(uint32_t)}};
  const VkSpecializationInfo specialization{9, entries, sizeof(enabled), enabled};
  const VkComputePipelineCreateInfo create{
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main",
                .pSpecializationInfo = &specialization},
      .layout = source.layout};
  if (prefetch)
    pipeline::prefetchComputePipelines(device, 1, &create, true);
  else
    result = pipeline::createComputePipelines(device, VK_NULL_HANDLE, 1, &create, nullptr, out);
  pipeline::destroyShaderModule(device, module, nullptr);
  return result;
}

void PathTracer::Gpu::prewarmLightingVariants(LightingPipeline &source) {
  const auto &o = lightingOptions;
  const uint32_t earlyOut = (o.shadowEarlyOut ? variantEarlyOut : 0u) |
                            (source.baseSeparateIndirect ? variantSeparateIndirect : 0u);
  const uint32_t base = variantLocal | (source.baseEarlyOut ? variantEarlyOut : 0u) |
                        (source.baseSeparateIndirect ? variantSeparateIndirect : 0u);
  const bool capable = source.supportsGuiding;
  // The split direct pass runs split, or combined in mode 3; the opaque
  // passes split when the scene has emissive lights.
  std::vector<uint32_t> roles{0u};
  if (capable && o.separateLocalLighting != 0)
    roles = source.splitDirect ? std::vector<uint32_t>{o.separateLocalLighting == 3 ? variantCombinedDirect : variantSplit}
                               : std::vector<uint32_t>{0u, variantSplit};
  std::vector<uint32_t> keys{earlyOut}; // no local lights
  for (const uint32_t guided : {0u, uint32_t(variantGuided)}) {
    if (guided && !(capable && o.localGuiding))
      continue;
    for (const uint32_t set : {0u, uint32_t(variantSmallSet), uint32_t(variantResampling)}) {
      if (set == variantSmallSet && !capable)
        continue;
      if (set == variantResampling && !(capable && std::max(o.emissiveCandidates, o.sphereCandidates) > 1))
        continue;
      for (const uint32_t role : roles)
        keys.push_back(variantLocal | earlyOut | guided | set | role);
    }
  }
  // Likeliest first: scenes with many lights guide and resample; few lights
  // take the small set; none is the rarest.
  const auto rank = [](uint32_t key) {
    if (!(key & variantLocal)) return 5;
    if (key & variantSmallSet) return 4;
    const bool guided = key & variantGuided, resampling = key & variantResampling;
    return guided && resampling ? 0 : guided || resampling ? 1 : 2;
  };
  std::stable_sort(keys.begin(), keys.end(), [&](uint32_t a, uint32_t b) { return rank(a) < rank(b); });
  for (const auto key : keys) {
    if (key == base)
      continue;
    auto &[variant, attempted] = source.specialized[key];
    if (variant || attempted)
      continue;
    VkPipeline none{};
    requestLightingVariant(source, key, &none, true);
  }
}

void PathTracer::Gpu::bindTrace(VkCommandBuffer command, VkPipeline pipeline, VkPipelineLayout pipelineLayout,
                                uint32_t features) {
  vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
  vkCmdPushConstants(command, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, featuresOffset, sizeof(features), &features);
}

void PathTracer::Gpu::forgetLightingPipeline(VkPipeline pipeline) {
  const auto found = lightingPipelines.find(pipeline);
  if (found == lightingPipelines.end())
    return;
  for (const auto &[key, variant] : found->second.specialized)
    if (variant.first)
      pipeline::destroyPipeline(device, variant.first, nullptr);
  lightingPipelines.erase(found);
}
} // namespace remix
