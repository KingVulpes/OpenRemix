#pragma once
#include "denoise/renderer.h"
#include "pathtrace/options.h"
#include "postfx/renderer.h"
#include "raster/renderer.h"
#include "scene/trace_frame.h"
#include <remix/remix_native_ext.h>
#include <vulkan/vulkan.h>

namespace remix {
  class PathTracer {
  public:
    static constexpr uint32_t textureLimit = 4096;
    PathTracer();
    ~PathTracer();
    void initialize(VkPhysicalDevice physical, VkDevice device,
                    uint32_t queueFamily, bool preparing = false,
                    bool rasterPrimary = true, std::shared_ptr<postfx::State> exposureState = {});
    std::shared_ptr<postfx::State> exposureState() const;
    VkExtent2D renderExtent() const;
    void setPostProcessing(postfx::Renderer *renderer, bool retainExposure = false);
    void configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput, VkFormat depthFormat,
                         VkImageView depthView, bool hardwareSrgb = false);
    VkRenderPass skyPass() const;
    upscale::Statistics upscalingStatistics() const;
    // What frame generation needs of the frame just prepared: the upscaler's
    // depth and motion and the cameras of this and the previous frame
    // (unjittered). Not valid without an upscaler.
    struct FrameGenerationInputs {
      upscale::FrameGenerationInputs images;
      scene::Matrix view{}, projection{}, previousView{}, previousProjection{};
      float frameMilliseconds = 0; // since the previous frame, as the upscaler was given
      bool reset = true; // no usable previous frame
      bool valid = false;
    };
    FrameGenerationInputs frameGeneration() const;
    void setNgx(std::shared_ptr<upscale::Ngx> ngx);
    void prepareConfiguration(const TraceOptions &options, bool perspective,
                              bool local, uint32_t failedBackends = 0,
                              const std::string &failure = {});
    bool finishPreparation(VkQueue queue);
    void beginFrame(const TraceOptions &options, const RasterRenderer &resources);
    void selectFrame(uint32_t slot);
    void submitted();
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    remixapi_LocalLightStatisticsEXT localLightStatistics() const;
    remixapi_LocalLightGuidingStatisticsEXT localLightGuidingStatistics() const;
    remixapi_LocalLightSetStatisticsEXT localLightSetStatistics() const;
    remixapi_CameraCullingStatisticsEXT cameraCullingStatistics() const;
    remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
    bool translucencyDenoiserActive() const;
    remixapi_SharcStatisticsEXT sharcStatistics() const;
    void destroyTarget();
    // Traces the resources' persistent static set (staticDrawn) from the
    // tracer's own copy of it; may expand the set into the draw list when a
    // frame cannot (NativeResources::expandStatic).
    scene::TraceFrame prepareScene(RasterRenderer &resources, const TraceOptions &options) const;
    void prepare(VkCommandBuffer command, RasterRenderer &resources, const TraceOptions &options,
                 scene::TraceFrame &scene);
    void record(VkCommandBuffer command);
    remixapi_TracingStatisticsEXT statistics() const;
    bool adaptiveTerminationAvailable() const;
    void resetPerformance();
    remixapi_FramePerformanceEXT performance() const;
    // Phases of prepare/record for FrameBreakdownEXT; reset with resetPerformance.
    remixapi_FrameBreakdownEXT breakdown() const;
    // REMIX_FRAME_DIGEST diagnostics: an address-free hash of the last
    // prepared frame's GPU inputs (surface table, instances, primary draws).
    uint64_t frameDigest() const;
    // The persistent static set in the last prepared frame (Diagnostics).
    struct StaticStatistics {
      bool active = false;                      // traced from the kept static set
      uint64_t units = 0, surfaces = 0;         // its traced instances and table entries
      uint64_t reusedSurfaces = 0;              // static entries taken from kept tables
      uint64_t rebuiltSurfaces = 0;             // static entries of units built this frame
      uint64_t dynamicSurfaces = 0;             // entries of the frame's other instances
      uint64_t rebuiltUnits = 0, changes = 0, temporalSurfaces = 0, relayouts = 0;
      double syncMilliseconds = 0;              // within scene preparation
      double tableMilliseconds = 0;             // within trace surfaces
      uint64_t validations = 0, mismatches = 0; // "persistentStaticValidation"
      char mismatch[128] = {};                  // the first mismatch found
    };
    StaticStatistics staticStatistics() const;
    // Rebuilds the kept static tables from scratch every frame and compares.
    void setStaticValidation(bool enabled);

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix
