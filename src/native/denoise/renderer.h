#pragma once
#include "scene/types.h"
#include "upscale/renderer.h"
#include <remix/remix_native_ext.h>
#include <vulkan/vulkan.h>
#include <memory>
#include "platform/gpu_frames.h"

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
    BaseImageCount,
    Penumbra = BaseImageCount,
    ShadowInput,
    ShadowOutput,
    SigmaImageCount,
    LocalInput = SigmaImageCount,
    LocalOutput,
    LocalImageCount,
    EmissiveInput = LocalImageCount,
    EmissiveOutput,
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
    float sun[4] = {};       // direction toward sun, SIGMA active
    uint32_t shadow[4] = {}; // SIGMA reset, debug view, reserved
    uint32_t local[4] = {}; // denoise, active lights, responsive frames, reserved
    uint32_t families[4] = {}; // torch/emissive counts, torch/emissive response frames
  };
  static_assert(sizeof(Frame) == 464 && offsetof(Frame, families) == 448 && offsetof(Frame, local) == 432 && offsetof(Frame, sun) == 400 && offsetof(Frame, shadow) == 416);
  remixapi_DenoisingCapabilitiesEXT capabilities(VkPhysicalDevice physical);
  bool sigmaSupported(VkPhysicalDevice physical);
  bool localSupported(VkPhysicalDevice physical, bool sigma, bool split = false);
  class Renderer {
  public:
    Renderer();
    ~Renderer();
    void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D extent, bool reblur,
                    uint32_t queueFamily, bool sigma = false, bool local = false, bool localDenoise = false, bool split = false);
    VkDescriptorSetLayout inputLayout() const;
    VkDescriptorSet inputs() const;
    void selectFrame(uint32_t slot);
    upscale::Input image(ImageSlot slot) const;
    void begin(VkCommandBuffer command, const Frame &frame, const scene::Matrix &projection,
               const scene::Matrix &previousProjection, VkImageView world, VkImageView sky);
    void beginConfidence(VkCommandBuffer command);
    void record(VkCommandBuffer command);
    void submitted();
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    float localMilliseconds() const;
    remixapi_DenoisingStatisticsEXT statistics() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix::denoise
