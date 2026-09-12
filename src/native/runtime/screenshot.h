#pragma once
#include <remix/remix_c.h>
#include <vulkan/vulkan.h>
#include <string>

namespace remix {
  // One on-demand readback. All Vulkan access is on the presentation thread.
  class PresentedScreenshot {
  public:
    remixapi_ErrorCode request(const char *path, bool supported);
    void record(VkPhysicalDevice physical, VkDevice device, VkCommandBuffer command, VkImage image,
                VkExtent2D extent, VkFormat format);
    void submitted(VkFence fence);
    void complete(VkDevice device, bool wait = false);
    void reset(VkDevice device, bool keepRequest = false);

  private:
    std::string m_requested, m_captured;
    VkBuffer m_buffer = VK_NULL_HANDLE;
    VkDeviceMemory m_memory = VK_NULL_HANDLE;
    VkExtent2D m_extent = {};
    bool m_bgra = false;
    VkFence m_fence = VK_NULL_HANDLE;
  };
} // namespace remix
