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
    remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
    bool translucencyDenoiserActive() const;
    remixapi_SharcStatisticsEXT sharcStatistics() const;
    void destroyTarget();
    scene::TraceFrame prepareScene(const RasterRenderer &resources, const TraceOptions &options) const;
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

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix
