#pragma once
#include <vulkan/vulkan.h>
#include "scene/resources.h"
#include "platform/gpu_frames.h"

namespace remix {
  class RasterRenderer : public NativeResources {
  public:
    RasterRenderer();
    ~RasterRenderer();
    // textureCompression: the device was created with textureCompressionBC, so
    // compressed textures upload as BC images instead of a decoded fallback.
    void initializeGpu(VkPhysicalDevice physical, VkDevice device, bool textureCompression = false,
                       bool memoryBudget = false);
    // Takes other's GPU state (caches, pipelines, target and raster profile),
    // leaving other without one. Asynchronous presentation renders from a
    // published frame (NativeResources::publishFrame) held by a second
    // renderer; the caller has completed the render thread's work.
    void adoptGpu(RasterRenderer &other);
    struct MemoryStatistics {
      uint64_t textureBytes = 0, textureCount = 0, compressedTextureBytes = 0;
      uint64_t geometryBytes = 0, pooledBytes = 0;
    };
    MemoryStatistics memoryStatistics() const; // Device allocations owned by this renderer.
    // CPU cost of the last prepare() (FrameBreakdownEXT).
    struct Profile {
      double prepareMilliseconds = 0, skinningMilliseconds = 0;
      uint64_t draws = 0, skinnedVertices = 0, uploadBytes = 0; // draws include halo sources
      uint64_t haloGroups = 0, haloDraws = 0;
      uint64_t maskGroups = 0, maskDraws = 0; // groups with coverage, their source draws
      uint64_t digest = 0; // REMIX_FRAME_DIGEST only: commands and skinned vertices.
    };
    const Profile &profile() const { return m_profile; }
    // linearTarget: the target has an sRGB format (fixed-function blending is
    // linear); otherwise blending operates on display-referred values.
    void configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput = false,
                         bool linearTarget = false);
    void setSkyTarget(VkRenderPass pass);
    void destroyTarget(); // Caller has completed all submitted work.
    void shutdownGpu();
    void selectFrame(uint32_t slot); // Caller has completed this slot's submission.
    VkFormat depthFormat() const;
    VkImageView depthView() const;
    std::shared_ptr<void> depthLifetime() const;
    bool preparePipelines(bool tracing);
    void prepare(VkCommandBuffer command,
                 bool tracing = false); // After prior frame fence, outside render pass.
    // Layers: 0 sky, 1 world, 2 view model, 3 UI and overlays. part: with
    // masked overlays, the commands before (1) or from (2) the coverage passes.
    void record(VkCommandBuffer command, bool skyOnly = false, bool skipSky = false,
                const float *skyJitter = nullptr, VkExtent2D skyExtent = {}, int firstLayer = 0,
                int lastLayer = 3, int part = 0); // Inside render pass, before ImGui.

    // Masked overlays (remixapi_InstanceInfoMaskEXT, masked UI commands).
    // maskRuns: coverage passes the last prepare() made; zero when the frame
    // has none. Such a frame records layers 0-1 part 1 (every world draw that
    // is not masked), leaves the target pass (its depth, stored, is what
    // depth-tested sources test), recordMasks (own passes, one per run),
    // re-enters a pass that loads colour and depth and records layers 0-1
    // part 2 (the masked world overlays and the blended draws sorted after the
    // first of them); then halos, then layers 2-3.
    uint32_t maskRuns() const;
    void recordMasks(VkCommandBuffer command);

    // Halo outlines (remixapi_InstanceInfoHaloEXT). haloCount: groups the last
    // prepare() made ready, in composite order; zero when the frame has none.
    // A frame with halos records layers 0-1, then per group: leaves the target
    // pass (its depth attachment, stored, holds the scene's depth),
    // recordHaloMask (own passes: silhouette mask, blur), re-enters a pass on
    // the target that loads its colour and recordHaloComposite; then layers
    // 2-3. The view model and the UI therefore draw over halos.
    uint32_t haloCount() const;
    void recordHaloMask(VkCommandBuffer command, uint32_t group);
    void recordHaloComposite(VkCommandBuffer command, uint32_t group);

    VkDescriptorImageInfo sampledMaterial(const std::shared_ptr<scene::Material> &material,
                                          VkCommandBuffer command);
    VkDescriptorImageInfo sampledEmission(const std::shared_ptr<scene::Material> &material,
                                          VkCommandBuffer command);
    VkDescriptorImageInfo sampledTexture(const std::shared_ptr<scene::Material> &material,
                                         const std::shared_ptr<scene::Texture> &texture,
                                         VkCommandBuffer command);

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
    Profile m_profile;
  };
} // namespace remix
