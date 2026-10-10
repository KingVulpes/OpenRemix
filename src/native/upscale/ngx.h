#pragma once
#include "upscale/renderer.h"
#include <mutex>
#include <vector>

namespace remix::upscale {
struct NgxCapabilities {
  bool sdk{}, sr{}, rr{}, initializationComplete{};
  std::string reason;
  // DLSS Frame Generation (NVSDK_NGX_Feature_FrameGeneration). `fgMultiFrameMax`:
  // generated frames per rendered frame the feature supports (1: 2x only).
  bool fg{};
  uint32_t fgMultiFrameMax{};
  std::string fgReason;
};
// Device lifetime; features hold a reference and must be released before shutdown.
class Ngx {
public:
  Ngx();
  ~Ngx();
  std::vector<std::string> instanceExtensions();
  std::vector<std::string> deviceExtensions(VkInstance instance, VkPhysicalDevice physical);
  void setDevice(VkInstance instance, VkPhysicalDevice physical,
                 VkDevice device);
  void initializeDevice();
  void initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
  NgxCapabilities capabilities() const;
  VkExtent2D renderSize(VkExtent2D display, uint32_t quality, bool rr);

private:
  friend class NgxFeature;
  friend class NgxFrameGeneration;
  struct Impl;
  std::unique_ptr<Impl> m;
  mutable std::mutex capabilitiesMutex;
  NgxCapabilities publishedCapabilities;
  VkInstance instance{};
  VkPhysicalDevice physical{};
  VkDevice device{};
  bool deviceAssigned = false;
};
class NgxFeature {
public:
  NgxFeature();
  ~NgxFeature();
  void initialize(std::shared_ptr<Ngx> context, uint32_t family, VkExtent2D render, VkExtent2D display,
                  uint32_t quality, bool rr);
  bool finishInitialization(VkQueue queue);
  uint64_t memoryBytes() const;
  void record(VkCommandBuffer command, Input color, Input depth, Input motion, Input output, Input exposure,
              const ReconstructionGuides &guides, const scene::Matrix &view, const scene::Matrix &projection,
              const float *jitter, float milliseconds, bool reset);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
// The camera of a frame as DLSS Frame Generation takes it. Matrices without
// jitter, in scene::Matrix storage (see NgxFrameGeneration::record).
struct FrameGenerationCamera {
  scene::Matrix viewToClip{}, clipToView{}, clipToPrevClip{}, prevClipToClip{};
  float position[3]{}, up[3]{}, right[3]{}, forward[3]{};
  float nearPlane{}, farPlane{}, fieldOfView{}, aspect{};
  bool depthInverted{}, reset{};
};
// DLSS Frame Generation (NVSDK_NGX_Feature_FrameGeneration): one generated
// frame between the previous and the current backbuffer.
class NgxFrameGeneration {
public:
  NgxFrameGeneration();
  ~NgxFrameGeneration();
  // `format`: of the backbuffer and output images; `render`: of depth and motion.
  void initialize(std::shared_ptr<Ngx> context, uint32_t family, VkExtent2D display, VkFormat format,
                  VkExtent2D render);
  bool finishInitialization(VkQueue queue);
  // Inputs in SHADER_READ_ONLY_OPTIMAL, `output` in GENERAL. `hudless`: the
  // backbuffer before its interface was drawn (no image: not provided).
  // `generated` frames are made between the two rendered ones; `index` (from
  // one) is the one to write.
  void record(VkCommandBuffer command, Input backbuffer, Input depth, Input motion, Input hudless, Input output,
              const FrameGenerationCamera &camera, uint32_t generated, uint32_t index);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
} // namespace remix::upscale
