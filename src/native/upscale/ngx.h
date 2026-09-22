#pragma once
#include "upscale/renderer.h"
#include <vector>

namespace remix::upscale {
struct NgxCapabilities {
  bool sdk{}, sr{}, rr{};
  std::string reason;
};
// Device lifetime; features hold a reference and must be released before shutdown.
class Ngx {
public:
  Ngx();
  ~Ngx();
  std::vector<std::string> instanceExtensions();
  std::vector<std::string> deviceExtensions(VkInstance instance, VkPhysicalDevice physical);
  void initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device);
  NgxCapabilities capabilities() const;
  VkExtent2D renderSize(VkExtent2D display, uint32_t quality, bool rr);

private:
  friend class NgxFeature;
  struct Impl;
  std::unique_ptr<Impl> m;
};
class NgxFeature {
public:
  NgxFeature();
  ~NgxFeature();
  void initialize(std::shared_ptr<Ngx> context, uint32_t family, VkExtent2D render, VkExtent2D display,
                  uint32_t quality, bool rr);
  uint64_t memoryBytes() const;
  void record(VkCommandBuffer command, Input color, Input depth, Input motion, Input output, Input exposure,
              const ReconstructionGuides &guides, const scene::Matrix &view, const scene::Matrix &projection,
              const float *jitter, float milliseconds, bool reset);

private:
  struct Impl;
  std::unique_ptr<Impl> m;
};
} // namespace remix::upscale
