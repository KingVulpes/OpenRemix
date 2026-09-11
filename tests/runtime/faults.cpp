#include <cstdlib>
#include <algorithm>
#include <set>
#include <cstring>
#include <dlfcn.h>
#include <vulkan/vulkan.h>

namespace {
  std::set<VkShaderModule> fsrModules;
  bool armed = false;
  bool computeCreated = false;
  uint32_t computeAttempts = 0;
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
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice device,
    const VkDescriptorSetLayoutCreateInfo *info, const VkAllocationCallbacks *alloc, VkDescriptorSetLayout *output) {
  if (fault("local-descriptors"))
    for (uint32_t i = 0; i < info->bindingCount; ++i)
      if (info->pBindings[i].binding == 29 && info->pBindings[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) {
        *output = VK_NULL_HANDLE;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
      }
  return reinterpret_cast<PFN_vkCreateDescriptorSetLayout>(resolve("vkCreateDescriptorSetLayout"))(device, info, alloc, output);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice device,
    const VkShaderModuleCreateInfo* info, const VkAllocationCallbacks* alloc, VkShaderModule* output) {
  auto result = reinterpret_cast<PFN_vkCreateShaderModule>(resolve("vkCreateShaderModule"))(device,info,alloc,output);
  const char marker[] = "cbFSR3Upscaler";
  const auto* begin = reinterpret_cast<const char*>(info->pCode);
  if(result == VK_SUCCESS && std::search(begin,begin+info->codeSize,marker,marker+sizeof(marker)-1)!=begin+info->codeSize)
    fsrModules.insert(*output);
  return result;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice device,VkShaderModule module,const VkAllocationCallbacks* alloc) {
  fsrModules.erase(module);
  reinterpret_cast<PFN_vkDestroyShaderModule>(resolve("vkDestroyShaderModule"))(device,module,alloc);
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateComputePipelines(VkDevice device, VkPipelineCache cache,
                                                                   uint32_t count,
                                                                   const VkComputePipelineCreateInfo *infos,
                                                                   const VkAllocationCallbacks *alloc,
                                                                   VkPipeline *outputs) {
  ++computeAttempts;
  if (fault("fsr-pipeline")) for(uint32_t i=0;i<count;++i) if(fsrModules.count(infos[i].stage.module)) {
    for(uint32_t j=0;j<count;++j)outputs[j]=VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  if (fault("light-guiding-compute")) {
    for (uint32_t i = 0; i < count; ++i) {
      const auto *specialization = infos[i].stage.pSpecializationInfo;
      if (!specialization)
        continue;
      for (uint32_t j = 0; j < specialization->mapEntryCount; ++j) {
        const auto &entry = specialization->pMapEntries[j];
        VkBool32 enabled = VK_FALSE;
        if (entry.constantID != 1 || entry.size != sizeof(enabled))
          continue;
        std::memcpy(&enabled, static_cast<const char *>(specialization->pData) + entry.offset, sizeof(enabled));
        if (enabled) {
          for (uint32_t k = 0; k < count; ++k)
            outputs[k] = VK_NULL_HANDLE;
          return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
      }
    }
  }
  if (fault("compute") || (fault("transparency-compute") && computeAttempts == 2) ||
      ((fault("sharc-compute") || fault("sigma-compute")) && computeAttempts == 3)) {
    for (uint32_t i = 0; i < count; ++i)
      outputs[i] = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  computeCreated = true;
  return reinterpret_cast<PFN_vkCreateComputePipelines>(resolve("vkCreateComputePipelines"))(
      device, cache, count, infos, alloc, outputs);
}
// The raw tracer pipeline exists before temporal images are allocated. Inject
// failure there so fallback is tested independently from tracing support. The
// primary G-buffer also has an RGBA16F image, but it is a color attachment.
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice device, const VkImageCreateInfo *info,
                                                        const VkAllocationCallbacks *alloc, VkImage *output) {
  if (fault("fsr-allocation") && info->format == VK_FORMAT_R16G16_SFLOAT && info->extent.width == 640) {
    *output = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  if (computeCreated && fault("denoise-allocation") && info->format == VK_FORMAT_R16G16B16A16_SFLOAT &&
      !(info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
    *output = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  if (computeCreated && fault("sigma-allocation") && info->format == VK_FORMAT_R8G8B8A8_UNORM &&
      (info->usage & VK_IMAGE_USAGE_STORAGE_BIT) && !(info->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
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
  if (fault("sigma-descriptor-limit")) {
    properties->limits.maxPerStageDescriptorStorageImages = 33;
    properties->limits.maxDescriptorSetStorageImages = 33;
  }
  if (fault("temporal-descriptor-limit")) {
    properties->limits.maxPerStageDescriptorStorageImages = 27;
    properties->limits.maxDescriptorSetStorageImages = 27;
  }
}

extern "C" VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice device,
                                                                   VkPhysicalDeviceFeatures2 *features) {
  reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(resolve("vkGetPhysicalDeviceFeatures2"))(device, features);
  if (fault("sharc-capability")) {
    for (auto *next = static_cast<VkBaseOutStructure *>(features->pNext); next; next = next->pNext)
      if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        reinterpret_cast<VkPhysicalDeviceVulkan12Features *>(next)->shaderBufferInt64Atomics = VK_FALSE;
  }
}

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice device, const VkBufferCreateInfo *info,
                                                        const VkAllocationCallbacks *alloc, VkBuffer *output) {
  if (fault("light-guiding-allocation") && info->size == 1052672 &&
      (info->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
    *output = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  // The focused cache fixture uses 2^18 entries: fail its 16-byte accumulation
  // buffer after the hash buffer was allocated, exercising partial cleanup.
  if (fault("sharc-allocation") && info->size == (uint64_t(1) << 18) * 16 &&
      (info->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
    *output = VK_NULL_HANDLE;
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
  }
  return reinterpret_cast<PFN_vkCreateBuffer>(resolve("vkCreateBuffer"))(device, info, alloc, output);
}
