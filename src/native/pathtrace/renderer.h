#pragma once
#include <vulkan/vulkan.h>
#include "raster/renderer.h"
#include "denoise/renderer.h"
#include <remix/remix_native_ext.h>

namespace remix {
  struct TraceOptions {
    uint32_t samples = 1, bounces = 2;
    bool accumulate = false, forceRebuild = false;
    bool denoiser = true, taa = true, validation = false;
    bool fsr = false;
    bool prism = true, gamma22 = true;
    uint32_t fsrQuality = 1;
    float fsrSharpness = 0.f;
    bool sigma = true;
    bool localLights = true, localDenoiser = true;
    bool torchLights = true, emissiveLights = true;
    uint32_t emissiveCandidates = 4;
    uint32_t separateLocalLighting = 0; // combined, shared ray budget, one budget per family
    uint32_t localSamples = 1;
    float localIntensity = 1, localRange = 16;
    bool localGuiding = true;
    uint32_t shadowDebug = 0;
    float sunAngularDiameter = -1; // -1 inherits the distant light's diameter.
    bool rasterPrimary = true;
    bool waterIndirectReuse = true;
    bool sky = true;
    bool sharc = true, sharcDebug = false;
    uint32_t sharcCapacityLog2 = 22, sharcDownscale = 5, sharcSceneScale = 50;
    uint32_t sharcAccumulationFrames = 32, sharcStaleFrames = 64;
    uint64_t sharcReset = 0;
    bool overrideLight = false;
    float lightDirection[3] = {-0.4f, -0.8f, -0.3f};
    float lightColor[3] = {1, 1, 1};
    float lightIntensity = 3.14159265f;
  };
  class PathTracer {
  public:
    static constexpr uint32_t textureLimit = 512;
    PathTracer();
    ~PathTracer();
    void initialize(VkPhysicalDevice physical, VkDevice device, uint32_t queueFamily);
    void configureTarget(VkRenderPass pass, VkExtent2D extent, bool encodeOutput, VkFormat depthFormat,
                         VkImageView depthView, bool hardwareSrgb = false);
    VkRenderPass skyPass() const;
    upscale::Statistics upscalingStatistics() const;
    void beginFrame(const TraceOptions &options, const RasterRenderer& resources);
    void submitted();
    remixapi_ShadowStatisticsEXT shadowStatistics() const;
    remixapi_LocalLightStatisticsEXT localLightStatistics() const;
    remixapi_LocalLightGuidingStatisticsEXT localLightGuidingStatistics() const;
    remixapi_DenoisingStatisticsEXT denoisingStatistics() const;
    remixapi_SharcStatisticsEXT sharcStatistics() const;
    void destroyTarget();
    void prepare(VkCommandBuffer command, RasterRenderer &resources, const TraceOptions &options);
    void record(VkCommandBuffer command);
    remixapi_TracingStatisticsEXT statistics() const;

  private:
    struct Gpu;
    std::unique_ptr<Gpu> m_gpu;
  };
} // namespace remix
