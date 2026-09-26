#pragma once
#include "denoise/renderer.h"
#include "pathtrace/options.h"
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
    void initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily);
    void configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput, VkFormat depthFormat,
                         VkImageView depthView, bool hardwareSrgb = false);
    VkRenderPass skyPass() const;
    upscale::Statistics upscalingStatistics() const;
    void setNgx(std::shared_ptr<upscale::Ngx> ngx);
    void beginFrame(const TraceOptions &options, const RasterRenderer &resources);
    void selectFrame(uint32_t slot);
    void submitted();
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    remixapi_LocalLightStatisticsEXT localLightStatistics() const;
    remixapi_LocalLightGuidingStatisticsEXT localLightGuidingStatistics() const;
    remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
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

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix
