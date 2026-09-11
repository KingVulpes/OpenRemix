#pragma once
#include "scene/types.h"
#include <memory>
#include <string>
#include <vulkan/vulkan.h>
namespace remix::upscale {
struct Input {
  VkImage image{};
  VkImageView view{};
};
struct Statistics {
  bool active{};
  VkExtent2D render{}, display{};
  uint64_t frames{}, resets{};
  float milliseconds{};
  std::string failure;
};
bool supportsCamera(const scene::Matrix &projection);
VkExtent2D renderSize(VkExtent2D display, uint32_t quality);
void jitter(float *xy, uint64_t frame, VkExtent2D render, VkExtent2D display);
class Renderer {
public:
  Renderer();
  ~Renderer();
  void initialize(VkPhysicalDevice physical, VkDevice device,
                  uint32_t queueFamily, VkExtent2D render, VkExtent2D display);
  void configureComposite(VkRenderPass pass, VkImageView stableDepth);
  void composite(VkCommandBuffer command, bool encodeOutput);
  void record(VkCommandBuffer command, Input color, Input depth, Input motion,
              Input transmission, const scene::Matrix &projection,
              const float *jitter, float milliseconds, float sharpness,
              bool reset);
  Statistics statistics() const;

private:
  struct Gpu;
  std::unique_ptr<Gpu> m_gpu;
};
} // namespace remix::upscale
