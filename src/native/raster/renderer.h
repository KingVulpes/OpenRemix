#pragma once
#include <vulkan/vulkan.h>
#include "scene/resources.h"

namespace remix {
  class RasterRenderer : public NativeResources {
  public:
    RasterRenderer();
    ~RasterRenderer();
    void initializeGpu(VkPhysicalDevice physical, VkDevice device);
    void configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput = false);
    void setSkyTarget(VkRenderPass pass);
    void destroyTarget(); // Caller has completed all submitted work.
    void shutdownGpu();
    VkFormat depthFormat() const;
    VkImageView depthView() const;
    void prepare(VkCommandBuffer command,
                 bool tracing = false); // After prior frame fence, outside render pass.
    void record(VkCommandBuffer command, bool skyOnly = false, bool skipSky = false,
                const float *skyJitter = nullptr, VkExtent2D skyExtent = {}); // Inside render pass, before ImGui.

    VkDescriptorImageInfo sampledMaterial(const std::shared_ptr<scene::Material> &material,
                                          VkCommandBuffer command);

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix
