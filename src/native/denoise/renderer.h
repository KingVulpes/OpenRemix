#pragma once
#include "scene/types.h"
#include <remix/remix_native_ext.h>
#include <vulkan/vulkan.h>
#include <memory>

namespace remix::denoise {
  enum ImageSlot : uint32_t {
    Sh0,
    Sh1,
    Normal,
    Depth,
    Motion,
    Albedo,
    Direct,
    Transparent,
    Transmission,
    Out0,
    Out1,
    Confidence,
    GradientA,
    GradientB,
    History0,
    History1,
    PreviousDepth,
    PreviousNormal,
    PreviousLighting,
    Lighting,
    Composed,
    Validation,
    ImageCount
  };
  // Shared std140 constants. Matrices are nonjittered, in Vulkan framebuffer coordinates.
  struct Frame {
    scene::Matrix view{}, previousView{}, previousCamera{}, previousInverseCamera{}, inverseCamera{};
    float jitter[4] = {}; // current xy, previous zw, pixels
    float hitDistance[4] = {3.f, .1f, 20.f, 0};
    uint32_t flags[4] = {};  // reset, NRD, TAA, history parity
    uint32_t target[4] = {}; // width, height, previous seed, samples
    float timing[4] = {};    // frame milliseconds, validation, world changed, reserved
  };
  static_assert(sizeof(Frame) == 400);
  remixapi_DenoisingCapabilitiesEXT capabilities(VkPhysicalDevice physical);
  class Renderer {
  public:
    Renderer();
    ~Renderer();
    void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent, bool reblur,
                    uint32_t queueFamily);
    VkDescriptorSetLayout inputLayout() const;
    VkDescriptorSet inputs() const;
    void begin(VkCommandBuffer command, const Frame &frame, const scene::Matrix &projection,
               const scene::Matrix &previousProjection, VkImageView world, VkImageView sky);
    void beginConfidence(VkCommandBuffer command);
    void record(VkCommandBuffer command);
    void submitted();
    remixapi_DenoisingStatisticsEXT statistics() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::denoise
