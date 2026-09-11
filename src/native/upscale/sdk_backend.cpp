#include "upscale/sdk_backend.h"
#include <string_view>
namespace remix::upscale {
namespace {
thread_local ContextAllocations *allocations{};
template <auto Create, auto Destroy> struct Object;
template <class Info, class Handle,
          VkResult(VKAPI_PTR Create)(VkDevice, const Info *,
                                     const VkAllocationCallbacks *, Handle *),
          void(VKAPI_PTR Destroy)(VkDevice, Handle,
                                  const VkAllocationCallbacks *)>
struct Object<Create, Destroy> {
  static VKAPI_ATTR VkResult VKAPI_CALL
  create(VkDevice device, const Info *info,
         const VkAllocationCallbacks *allocator, Handle *handle) {
    auto result = Create(device, info, allocator, handle);
    if (result != VK_SUCCESS)
      throw result;
    if (allocations) {
      const auto value = *handle;
      allocations->add({uint64_t(value), uintptr_t(Destroy)},
                       [=] { Destroy(device, value, allocator); });
    }
    return result;
  }
  static VKAPI_ATTR void VKAPI_CALL destroy(
      VkDevice device, Handle handle, const VkAllocationCallbacks *allocator) {
    if (allocations)
      allocations->remove({uint64_t(handle), uintptr_t(Destroy)});
    Destroy(device, handle, allocator);
  }
};
VKAPI_ATTR VkResult VKAPI_CALL
createPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
                const VkComputePipelineCreateInfo *infos,
                const VkAllocationCallbacks *allocator, VkPipeline *pipelines) {
  auto result = vkCreateComputePipelines(device, cache, count, infos, allocator,
                                         pipelines);
  if (allocations)
    for (uint32_t i = 0; i < count; ++i)
      if (pipelines[i]) {
        const auto pipeline = pipelines[i];
        allocations->add(
            {uint64_t(pipeline), uintptr_t(vkDestroyPipeline)},
            [=] { vkDestroyPipeline(device, pipeline, allocator); });
      }
  if (result != VK_SUCCESS)
    throw result;
  return result;
}
VKAPI_ATTR void VKAPI_CALL
destroyPipeline(VkDevice device, VkPipeline pipeline,
                const VkAllocationCallbacks *allocator) {
  if (allocations)
    allocations->remove({uint64_t(pipeline), uintptr_t(vkDestroyPipeline)});
  vkDestroyPipeline(device, pipeline, allocator);
}
} // namespace
ContextAllocations::ContextAllocations() { allocations = this; }
ContextAllocations::~ContextAllocations() {
  allocations = nullptr;
  for (auto i = order.rbegin(); i != order.rend(); ++i) {
    auto found = live.find(*i);
    if (found != live.end()) {
      found->second();
      live.erase(found);
    }
  }
}
void ContextAllocations::commit() {
  live.clear();
  order.clear();
  allocations = nullptr;
}
void ContextAllocations::add(Key handle, std::function<void()> destroy) {
  live[handle] = std::move(destroy);
  order.push_back(handle);
}
void ContextAllocations::remove(Key handle) { live.erase(handle); }
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL sdkDeviceProc(VkDevice device,
                                                       const char *name) {
  const std::string_view symbol(name);
#define OBJECT(createFn, destroyFn)                                            \
  if (symbol == #createFn)                                                     \
    return reinterpret_cast<PFN_vkVoidFunction>(                               \
        &Object<createFn, destroyFn>::create);                                 \
  if (symbol == #destroyFn)                                                    \
    return reinterpret_cast<PFN_vkVoidFunction>(                               \
        &Object<createFn, destroyFn>::destroy);
  OBJECT(vkAllocateMemory, vkFreeMemory)
  OBJECT(vkCreateBuffer, vkDestroyBuffer)
  OBJECT(vkCreateBufferView, vkDestroyBufferView)
  OBJECT(vkCreateImage, vkDestroyImage)
  OBJECT(vkCreateImageView, vkDestroyImageView)
  OBJECT(vkCreateShaderModule, vkDestroyShaderModule)
  OBJECT(vkCreatePipelineLayout, vkDestroyPipelineLayout)
  OBJECT(vkCreateDescriptorPool, vkDestroyDescriptorPool)
  OBJECT(vkCreateDescriptorSetLayout, vkDestroyDescriptorSetLayout)
  OBJECT(vkCreateSampler, vkDestroySampler)
#undef OBJECT
  if (symbol == "vkCreateComputePipelines")
    return reinterpret_cast<PFN_vkVoidFunction>(createPipelines);
  if (symbol == "vkDestroyPipeline")
    return reinterpret_cast<PFN_vkVoidFunction>(destroyPipeline);
  // The SDK uses the extension spelling of this Vulkan 1.1 core entry point.
  if (symbol == "vkGetBufferMemoryRequirements2KHR")
    return vkGetDeviceProcAddr(device, "vkGetBufferMemoryRequirements2");
  return vkGetDeviceProcAddr(device, name);
}
} // namespace remix::upscale
