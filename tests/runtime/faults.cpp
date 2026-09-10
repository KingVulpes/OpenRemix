#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

namespace {
  bool armed = false;
  bool computeCreated = false;
  void *resolve(const char *name) {
    static void *loader = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    return dlsym(loader, name);
  }
  bool fault(const char *name) {
    const char *value = std::getenv("REMIX_TEST_FAULT");
    return value && std::strcmp(value, name) == 0;
  }
} // namespace
// Explicit signatures keep this loader independent of the shell implementation.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *info,
                                                           const VkAllocationCallbacks *alloc,
                                                           VkInstance *output) {
  if (fault("instance")) {
    *output = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  return reinterpret_cast<PFN_vkCreateInstance>(resolve("vkCreateInstance"))(info, alloc, output);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice device,
                                                         const VkDeviceCreateInfo *info,
                                                         const VkAllocationCallbacks *alloc,
                                                         VkDevice *output) {
  if (fault("device")) {
    *output = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  return reinterpret_cast<PFN_vkCreateDevice>(resolve("vkCreateDevice"))(device, info, alloc, output);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice device,
                                                              const VkFramebufferCreateInfo *info,
                                                              const VkAllocationCallbacks *alloc,
                                                              VkFramebuffer *output) {
  if (fault("framebuffer")) {
    *output = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  return reinterpret_cast<PFN_vkCreateFramebuffer>(resolve("vkCreateFramebuffer"))(device, info, alloc,
                                                                                   output);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache,
                                                                   uint32_t count,
                                                                   const VkComputePipelineCreateInfo *infos,
                                                                   const VkAllocationCallbacks *alloc,
                                                                   VkPipeline *outputs) {
  if (fault("compute")) {
    for (uint32_t i = 0; i < count; ++i)
      outputs[i] = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  computeCreated = true;
  return reinterpret_cast<PFN_vkCreateComputePipelines>(resolve("vkCreateComputePipelines"))(
      device, cache, count, infos, alloc, outputs);
}
// The raw tracer pipeline exists before temporal images are allocated. Inject
// failure there so fallback is tested independently from tracing support.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
                                                        const VkAllocationCallbacks *alloc, VkImage *output) {
  if (computeCreated && fault("denoise-allocation") && info->format == VK_FORMAT_R16G16B16A16_SFLOAT) {
    *output = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  return reinterpret_cast<PFN_vkCreateImage>(resolve("vkCreateImage"))(device, info, alloc, output);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice device, uint32_t count,
                                                        const VkFence *fences) {
  armed = true;
  return reinterpret_cast<PFN_vkResetFences>(resolve("vkResetFences"))(device, count, fences);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t count,
                                                        const VkSubmitInfo *submits, VkFence fence) {
  if (armed && (fault("submit") || fault("device-lost"))) {
    armed = false;
    return fault("device-lost") ? VK_ERROR_DEVICE_LOST : VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  armed = false;
  return reinterpret_cast<PFN_vkQueueSubmit>(resolve("vkQueueSubmit"))(queue, count, submits, fence);
}

namespace {
  bool recovered = false;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                                uint64_t timeout, VkSemaphore semaphore,
                                                                VkFence fence, uint32_t *image) {
  if (!recovered && (fault("acquire-out-of-date") || fault("surface-lost"))) {
    recovered = true;
    return fault("surface-lost") ? VK_ERROR_SURFACE_LOST_KHR : VK_ERROR_OUT_OF_DATE_KHR;
  }
  auto result = reinterpret_cast<PFN_vkAcquireNextImageKHR>(resolve("vkAcquireNextImageKHR"))(
      device, swapchain, timeout, semaphore, fence, image);
  if (!recovered && fault("acquire-suboptimal") && result == VK_SUCCESS) {
    recovered = true;
    return VK_SUBOPTIMAL_KHR;
  }
  return result;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *info) {
  auto result = reinterpret_cast<PFN_vkQueuePresentKHR>(resolve("vkQueuePresentKHR"))(queue, info);
  if (!recovered && fault("present-suboptimal") && result == VK_SUCCESS) {
    recovered = true;
    return VK_SUBOPTIMAL_KHR;
  }
  return result;
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice device,
                                                                    VkPhysicalDeviceProperties *properties) {
  reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(resolve("vkGetPhysicalDeviceProperties"))(device,
                                                                                                properties);
  if (fault("temporal-descriptor-limit")) {
    properties->limits.maxPerStageDescriptorStorageImages = 27;
    properties->limits.maxDescriptorSetStorageImages = 27;
  }
}
