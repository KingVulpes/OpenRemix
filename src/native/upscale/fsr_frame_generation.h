#pragma once
#include "upscale/renderer.h"
#include <memory>

namespace remix::upscale {
// FSR 3.1 frame generation: AMD's optical flow and frame interpolation
// effects, one generated frame halfway between the previous and the current
// backbuffer. The counterpart of NgxFrameGeneration, on any GPU.
//
// It takes depth and motion vectors directly (the interpolation's own prepare
// stage dilates them), not the FSR upscaler's shared resources, so it runs
// behind either upscaler. AMD's frame-pacing swapchain is not used: the
// runtime presents the frames.
class FsrFrameGeneration {
public:
  FsrFrameGeneration();
  ~FsrFrameGeneration();
  // `format`: of the backbuffer, HUD-less and output images (8-bit RGBA or
  // BGRA holding the sRGB-encoded frame); `render`: of depth and motion;
  // `depthInverted`: of the depth record() is given, which selects a shader
  // permutation. Compiles the effects' pipelines: call on the pipeline
  // service's worker (runtime/pipelines.h). Throws without leaving anything
  // behind.
  void initialize(VkPhysicalDevice physical, VkDevice device, VkExtent2D display, VkFormat format,
                  VkExtent2D render, bool depthInverted);
  // Nothing is deferred to the queue; present for the shape of NgxFrameGeneration.
  bool finishInitialization(VkQueue queue);
  // A rendered frame was shown without a record(): the kept previous frame is
  // not the one before the next record(), which therefore resets.
  void discontinuity();
  // `backbuffer`, `depth`, `motion` and `hudless` in SHADER_READ_ONLY_OPTIMAL,
  // `output` in GENERAL, all left so. `hudless`: the backbuffer before its
  // interface was drawn (no image: not provided). `motion`: in render pixels,
  // from a pixel of this frame to where it was in the previous one, without
  // jitter. `frameMilliseconds`: between the previous rendered frame and this one.
  void record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless, Input output,
              const FrameGenerationCamera &camera, float frameMilliseconds);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
} // namespace remix::upscale
