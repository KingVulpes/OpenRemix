#pragma once
#include "platform/gpu_frames.h"
#include "upscale/options.h"
#include "scene/types.h"
#include <memory>
#include <string>
#include <vulkan/vulkan.h>
namespace remix::upscale {
enum class Backend : uint32_t { Native, Fsr31, DlssSr, DlssRr };
const char *name(Backend backend);
class Ngx;
struct Input {
  VkImage image{};
  VkImageView view{};
};
struct ReconstructionGuides {
  Input diffuse, specular, normalRoughness, hitDistance;
};
struct Statistics {
  Backend backend{};
  Mode requested{};
  bool srSupported{}, rrSupported{}, sdkAvailable{}, failed{};
  uint64_t failures{}, memoryBytes{};
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
  void initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily, VkExtent2D render,
                  VkExtent2D display, Backend backend = Backend::Fsr31, std::shared_ptr<Ngx> ngx = {},
                  uint32_t quality = 1);
  void configureComposite(VkRenderPass pass, VkImageView stableDepth);
  void selectFrame(uint32_t slot);
  void composite(VkCommandBuffer command, bool encodeOutput, bool prism, bool gamma22, bool hardwareSrgb, VkDescriptorSet post);
  Input output() const;
  void record(VkCommandBuffer command, Input color, Input depth, Input motion, Input transmission,
              const scene::Matrix &projection, const float *jitter, float milliseconds, float sharpness,
              bool reset, const ReconstructionGuides &guides = {}, const scene::Matrix &view = {});
  Statistics statistics() const;
  bool finishInitialization(VkQueue queue);

private:
  struct Gpu;
  std::unique_ptr<Gpu> m_gpu;
};
} // namespace remix::upscale
