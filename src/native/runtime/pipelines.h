#pragma once
#include "runtime/preparation.h"
#include <chrono>
#include <vulkan/vulkan.h>

namespace remix::pipeline {
struct Statistics {
  uint64_t created{}, reused{}, deduplicated{}, completed{}, requested{},
      failed{}, cacheHits{}, driverNanoseconds{};
  double milliseconds{};
  bool feedback{}, cacheLoaded{}, cacheWriteFailed{};
};
// Owned by the runtime, outlives every renderer and all submitted work.
class Service {
public:
  Service(VkDevice device, const VkPhysicalDeviceProperties &properties,
          bool feedback);
  ~Service();
  Service(const Service &) = delete;
  Service &operator=(const Service &) = delete;
  PreparationQueue::Ticket prepare(const std::string &key,
                                   std::function<void()> work);
  void cancel(const PreparationQueue::Ticket &job);
  void stop();
  Statistics statistics() const;
  struct Impl;
  std::unique_ptr<Impl> impl;
};

// The wrappers intern immutable definitions, never images, descriptor pools or
// histories. A main-thread cache miss schedules work and returns VK_NOT_READY.
// The compiler worker can call these while preparing replacement resources.
#define REMIX_PIPELINE_OBJECT(Name, Type)                                      \
  VKAPI_ATTR VkResult VKAPI_CALL create##Name(                                 \
      VkDevice, const Vk##Name##CreateInfo *, const VkAllocationCallbacks *,   \
      Type *);                                                                 \
  VKAPI_ATTR void VKAPI_CALL destroy##Name(VkDevice, Type,                     \
                                           const VkAllocationCallbacks *);
REMIX_PIPELINE_OBJECT(ShaderModule, VkShaderModule)
REMIX_PIPELINE_OBJECT(DescriptorSetLayout, VkDescriptorSetLayout)
REMIX_PIPELINE_OBJECT(PipelineLayout, VkPipelineLayout)
REMIX_PIPELINE_OBJECT(Sampler, VkSampler)
REMIX_PIPELINE_OBJECT(RenderPass, VkRenderPass)
#undef REMIX_PIPELINE_OBJECT
VKAPI_ATTR VkResult VKAPI_CALL createComputePipelines(
    VkDevice, VkPipelineCache, uint32_t, const VkComputePipelineCreateInfo *,
    const VkAllocationCallbacks *, VkPipeline *);
// Schedules the compiles createComputePipelines would perform and returns
// without waiting; a later createComputePipelines of the same pipelines
// awaits them. Lets a worker queue independent batches before blocking.
VKAPI_ATTR void VKAPI_CALL prefetchComputePipelines(
    VkDevice, uint32_t, const VkComputePipelineCreateInfo *);
VKAPI_ATTR VkResult VKAPI_CALL createGraphicsPipelines(
    VkDevice, VkPipelineCache, uint32_t, const VkGraphicsPipelineCreateInfo *,
    const VkAllocationCallbacks *, VkPipeline *);
VKAPI_ATTR void VKAPI_CALL destroyPipeline(VkDevice, VkPipeline,
                                           const VkAllocationCallbacks *);
} // namespace remix::pipeline
