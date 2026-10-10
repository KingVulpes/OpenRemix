#pragma once
#include "pathtrace/renderer.h"
#include "upscale/ngx.h"
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace remix {
// DLSS Frame Generation ("rtx.native.frameGeneration"): one to three
// generated frames between each pair of rendered frames.
//
// The frame is rendered into its swapchain image as without it. Afterwards the
// finished image (scene, HUD and developer UI) is copied into an image NGX can
// sample, NGX interpolates between it and the previous one into a second
// image, guided by the upscaler's depth and motion vectors, and that is copied
// into a further swapchain image, once per generated frame. The runtime
// presents the generated images and then the rendered one, spread evenly over
// the frame (presentation.cpp).
//
// The HUD: the frame as it was before the UI layer was drawn (captureHudless)
// tells NGX which pixels are interface, which it then keeps from the rendered
// frames instead of moving them with the scene.
//
// Owns the NGX feature and its images; sized to one swapchain extent, format
// and render extent (matches()).
class FrameGeneration {
public:
  static constexpr uint32_t maxGenerated = 3; // 4x
  FrameGeneration(std::shared_ptr<upscale::Ngx> ngx, VkPhysicalDevice physical, VkDevice device,
                  uint32_t queueFamily, VkExtent2D display, VkFormat swapchainFormat, VkExtent2D render);
  ~FrameGeneration();
  FrameGeneration(const FrameGeneration &) = delete;
  FrameGeneration &operator=(const FrameGeneration &) = delete;
  bool matches(VkExtent2D display, VkFormat swapchainFormat, VkExtent2D render) const;
  // Submits the feature's creation commands once and reports whether they
  // have completed (the caller owns the queue).
  bool ready(VkQueue queue);
  // Between render passes, before the UI layer: `real` (PRESENT_SRC, as a
  // pass leaves it) holds the frame without its HUD. Optional, once a frame.
  void captureHudless(VkCommandBuffer command, VkImage real);
  // `real` holds the finished frame; `generated` are the `count` further
  // acquired images (undefined contents), in the order they are shown. All are
  // left in PRESENT_SRC. Throws when NGX refuses an evaluation; the images
  // are then as duplicate() leaves them.
  void record(VkCommandBuffer command, VkImage real, const VkImage *generated, uint32_t count,
              const PathTracer::FrameGenerationInputs &inputs);
  // No interpolation this frame: the `generated` images become copies of `real`.
  void duplicate(VkCommandBuffer command, VkImage real, const VkImage *generated, uint32_t count);

private:
  struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
  };
  Target create(VkPhysicalDevice physical, VkImageUsageFlags usage) const;
  void destroy(Target &target) const;

  VkDevice m_device = VK_NULL_HANDLE;
  VkExtent2D m_display{}, m_render{};
  VkFormat m_swapchainFormat = VK_FORMAT_UNDEFINED, m_format = VK_FORMAT_UNDEFINED;
  Target m_backbuffer, m_interpolated, m_hudless;
  upscale::NgxFrameGeneration m_feature;
  bool m_first = true;           // no previous frame yet: NGX is told to reset
  bool m_hudlessCaptured = false; // this frame (cleared by record and duplicate)
};
// The swapchain images of a frame's generated frames, in the order shown.
struct GeneratedImages {
  uint32_t index[FrameGeneration::maxGenerated] = {};
  uint32_t count = 0;
};
} // namespace remix
