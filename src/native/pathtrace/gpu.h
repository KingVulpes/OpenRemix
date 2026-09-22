#pragma once
#include "scene/opacity.h"
#include "pathtrace/renderer.h"
#include "pathtrace/buffer_cache.h"
#include "pathtrace/tlas_policy.h"
#include "scene/light_bvh.h"
#include "scene/emission.h"
#include "platform/timing.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <unordered_set>

namespace remix::trace::detail {
  using namespace scene;

  inline void check(VkResult result) {
    if (result != VK_SUCCESS)
      throw result;
  }
  inline VkDeviceSize align(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
  }
  struct Image {
    VkDevice device = VK_NULL_HANDLE;
    VkImage handle = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    ~Image() {
      if (view)
        vkDestroyImageView(device, view, nullptr);
      if (handle)
        vkDestroyImage(device, handle, nullptr);
      if (memory)
        vkFreeMemory(device, memory, nullptr);
    }
  };
  struct Arena {
    std::unique_ptr<Buffer> buffer;
    std::map<VkDeviceSize, VkDeviceSize> free;
    void release(VkDeviceSize offset, VkDeviceSize size) {
      auto next = free.lower_bound(offset);
      if (next != free.begin()) {
        auto before = std::prev(next);
        if (before->first + before->second == offset) {
          offset = before->first;
          size += before->second;
          free.erase(before);
        }
      }
      if (next != free.end() && offset + size == next->first) {
        size += next->second;
        free.erase(next);
      }
      free.emplace(offset, size);
    }
  };
  struct Accel {
    VkDevice device;
    PFN_vkDestroyAccelerationStructureKHR destroy;
    std::shared_ptr<Arena> arena;
    VkDeviceSize offset = 0, size = 0;
    VkAccelerationStructureKHR handle = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    ~Accel() {
      if (handle)
        destroy(device, handle, nullptr);
      if (arena)
        arena->release(offset, size);
    }
  };
  struct TraceVertex {
    float position[4] = {};
    float uv[4] = {};
    float color[4] = {};
  };
  struct TraceSurface {
    uint64_t vertices = 0, indices = 0;
    float tint[4] = {}, uv[4] = {}, factor[4] = {};
    uint32_t state[4] = {}, textureState[4] = {};
    uint64_t triangleMap = 0, traceIndices = 0;
    uint64_t previousVertices = 0, debugIdentity = 0;
    Matrix previousModel{};
    float emission[4] = {};
    float pbr[4] = {}; // perceptual roughness, metalness, enabled, reserved
    uint32_t pbrTextures[4] = {}; // roughness, metalness, normal; index + 1
  };
  static_assert(sizeof(TraceSurface) == 240 && sizeof(TraceVertex) == 48);
  struct Frame {
    Matrix inverseCamera, camera;
    float eye[4] = {}, direction[4] = {}, radiance[4] = {};
    uint32_t config[4] = {}, target[4] = {};
    float jitter[4] = {}; // primary offset; z water reuse, w blue-noise sun/local bits
    float skyRadiance[4] = {}; // linear uniform environment; w reserved
    uint64_t sharcHash = 0, sharcAccumulation = 0, sharcResolved = 0, sharcCounters = 0;
    uint32_t sharcConfig[4] = {};
    float sharcGrid[4] = {}, sharcPreviousCamera[4] = {};
    float sun[4] = {}; // tangent of angular radius, SIGMA active, debug view, raw local-distance diagnostic
    uint64_t lightNodes = 0, lightEmitters = 0;
    uint32_t localConfig[4] = {};
    float localScale[4] = {};
    uint64_t lightGuideRead = 0, lightGuideWrite = 0;
    uint32_t lightGuideConfig[4] = {}; // capacity, node count, reserved
    uint64_t torchNodes = 0, torchEmitters = 0, emissiveNodes = 0, emissiveEmitters = 0;
    uint32_t splitLocal[4] = {}; // mode, torch count, emissive count, debug view
    uint64_t reuseRead = 0, reuseWrite = 0;
    uint32_t reuseConfig[4] = {}; // previous frame valid, diagnostics, ray counter address low/high
    uint64_t indirectResults = 0, blueNoise = 0;
  };
  static_assert(sizeof(Frame) == 512 && offsetof(Frame, indirectResults) == 496 && offsetof(Frame, lightGuideRead) == 384 && offsetof(Frame, lightGuideConfig) == 400 && offsetof(Frame, lightNodes) == 336 && offsetof(Frame, localConfig) == 352 && offsetof(Frame, localScale) == 368 && offsetof(Frame, skyRadiance) == 224 &&
                offsetof(Frame, sharcHash) == 240 && offsetof(Frame, sharcConfig) == 272 &&
                offsetof(Frame, sharcGrid) == 288 && offsetof(Frame, sharcPreviousCamera) == 304 && offsetof(Frame, sun) == 320);
  inline float linear(float c) { return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f); }
  inline void barrier(VkCommandBuffer command, VkPipelineStageFlags source, VkAccessFlags writes,
                      VkPipelineStageFlags destination, VkAccessFlags reads) {
    VkMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = writes, .dstAccessMask = reads};
    vkCmdPipelineBarrier(command, source, destination, 0, 1, &b, 0, nullptr, 0, nullptr);
  }
  constexpr VkBufferUsageFlags inputUsage =
      VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
  constexpr VkPipelineStageFlags buildStage = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
  constexpr VkAccessFlags asRead = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR;
  constexpr VkAccessFlags asWrite = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
} // namespace remix::trace::detail

namespace remix {
  struct PathTracer::Gpu {
    remixapi_FramePerformanceEXT performance{.structSize = sizeof(remixapi_FramePerformanceEXT)};
    std::vector<std::shared_ptr<trace::detail::BufferMemory>> bufferMemory;
    using Buffer = trace::detail::Buffer;
    using Image = trace::detail::Image;
    using Arena = trace::detail::Arena;
    using Accel = trace::detail::Accel;
    using TraceVertex = trace::detail::TraceVertex;
    using TraceSurface = trace::detail::TraceSurface;
    using Frame = trace::detail::Frame;
    struct GeometryScratch {
      std::string content;
      std::vector<std::vector<TraceVertex>> shading;
      std::vector<VkDeviceSize> vertexOffsets, indexOffsets;
      std::vector<uint32_t> counts, vertexCounts;
      size_t bytes() const {
        size_t size = content.capacity() + shading.capacity()*sizeof(std::vector<TraceVertex>) +
            (vertexOffsets.capacity()+indexOffsets.capacity())*sizeof(VkDeviceSize) +
            (counts.capacity()+vertexCounts.capacity())*sizeof(uint32_t);
        for (const auto &vertices : shading) size += vertices.capacity()*sizeof(TraceVertex);
        return size;
      }
    } conversion;

    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory = {};
    VkPhysicalDeviceProperties properties = {};
    VkPhysicalDeviceAccelerationStructurePropertiesKHR asProperties = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
    PFN_vkCreateAccelerationStructureKHR createAs = nullptr;
    PFN_vkDestroyAccelerationStructureKHR destroyAs = nullptr;
    PFN_vkGetAccelerationStructureDeviceAddressKHR getAsAddress = nullptr;
    PFN_vkGetBufferDeviceAddress getBufferAddress = nullptr;
    PFN_vkGetAccelerationStructureBuildSizesKHR buildSizes = nullptr;
    PFN_vkCmdBuildAccelerationStructuresKHR buildAs = nullptr;
    PFN_vkCmdCopyAccelerationStructureKHR copyAs = nullptr;
    PFN_vkCmdWriteAccelerationStructuresPropertiesKHR writeAsProperties = nullptr;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkPipeline transparencyCompute{}, temporalTransparencyCompute{};
    VkPipeline compute = VK_NULL_HANDLE, composite[2] = {};
    VkPipeline splitDirectCompute = VK_NULL_HANDLE;
    struct IndirectPass {
      std::unique_ptr<Buffer> results;
      VkPipeline pipeline[2] = {};
      bool active = false, failed = false;
      // Automatic selection targets combined lighting without SHARC queries.
      // The environment override can disable it or force other supported modes.
      uint32_t mode = [] {
        const auto *value = std::getenv("REMIX_SPLIT_INDIRECT");
        return !value ? 1u : std::strcmp(value, "1") == 0 ? 2u : 0u;
      }();
    } indirectPass;
    void configureIndirect(Frame &frame, const TraceOptions &options, bool querySharc);
    void destroyIndirect();
    struct LightingPipeline {
      const uint32_t *code = nullptr;
      size_t bytes = 0;
      VkPipelineLayout layout = VK_NULL_HANDLE;
      VkPipeline withoutLocal = VK_NULL_HANDLE;
      VkPipeline withGuiding = VK_NULL_HANDLE;
      VkPipeline withResampling[2] = {};
      bool resamplingAttempted[2] = {};
      std::map<uint32_t, std::pair<VkPipeline, bool>> specialized;
      bool attempted = false, guidingAttempted = false;
      bool supportsGuiding = true;
      bool preferFirstHit = true;
      VkPipeline smallSet[2] = {};
      bool smallAttempted[2] = {};
    };
    std::unordered_map<VkPipeline, LightingPipeline> lightingPipelines;
    struct RayProfile {
      std::unique_ptr<Buffer> counters, readback;
      bool pending = false;
      const char *path = std::getenv("REMIX_RAY_PROFILE");
    } rayProfile;
    void beginRayProfile(VkCommandBuffer command, Frame &frame);
    void finishRayProfile(VkCommandBuffer command);
    void completeRayProfile();
    struct PbrReuse {
      std::unique_ptr<Buffer> history[2], readback;
      uint32_t writeIndex = 0;
      uint64_t signature = 0;
      bool active = false, valid = false, pending = false, failed = false;
      const char *diagnostics = std::getenv("REMIX_PBR_REUSE_DIAGNOSTICS");
    } pbrReuse;
    void configurePbrReuse(VkCommandBuffer command, Frame &frame, const TraceOptions &options,
                           uint64_t signature, bool stable);
    void completePbrReuse();
    void finishPbrReuse(VkCommandBuffer command);
    bool localFirstHitActive = false;
    bool localIlluminationActive = false;
    bool localResamplingActive = false;
    bool localAngularActive = false;
    bool localGuidingActive = false;
    VkPipeline selectLightingPipeline(VkPipeline pipeline, bool combinedDirect = false, bool separateIndirect = false);
    void forgetLightingPipeline(VkPipeline pipeline);
    VkShaderModule shaders[3] = {};
    VkQueryPool compaction = VK_NULL_HANDLE, timestamps = VK_NULL_HANDLE;
    uint32_t timestampBits = 0;
    const char *stageProfile = std::getenv("REMIX_STACK_PROFILE");
    bool timingPending = false, imagesInitialized = false, encodeOutput = false;
    uint32_t debugView = 0;
    bool prism = true, gamma22 = true, hardwareSrgb = false;
    VkExtent2D extent = {}, displayExtent = {};
    VkRenderPass displayPass{};
    VkFormat displayDepthFormat{};
    VkImageView displayDepth{};
    std::shared_ptr<upscale::Ngx> ngx;
    upscale::Mode requestedUpscaler = upscale::Mode::Off;
    upscale::Backend requestedBackend = upscale::Backend::Native;
    bool rrRequested = true;
    uint32_t failedBackends = 0;
    uint64_t reconstructionFailures = 0, reconstructedFrames = 0, reconstructionResets = 0;
    bool rrActive() const { return upscaler && upscaler->statistics().backend == upscale::Backend::DlssRr; }
    TraceOptions effectiveOptions(TraceOptions options) const;
    upscale::Backend selectBackend() const;
    uint32_t fsrQuality = 1;
    std::unique_ptr<upscale::Renderer> upscaler;
    std::string fsrFailure;
    void createTarget();
    std::unique_ptr<Image> world, transmission, sky;
    struct PrimaryDraw {
      scene::Matrix model;
      uint32_t surfaceIndex = 0, count = 0;
      bool blended = false;
    };
    struct PrimaryPush {
      scene::Matrix model;
      float jitter[2] = {};
      uint32_t surfaceIndex = 0, stableDepth = 0;
    };
    static_assert(sizeof(PrimaryPush) == 80);
    std::vector<PrimaryDraw> primaryDraws;
    std::unique_ptr<Image> primary[7];
    VkRenderPass primaryPass[2] = {};
    VkFramebuffer primaryFramebuffer[2] = {};
    VkPipeline primaryPipeline[2] = {};
    VkPipelineLayout primaryLayout{};
    bool primaryInitialized = false;
    void createPrimary(VkFormat depthFormat, VkImageView depthView);
    void destroyPrimary();
    void recordPrimary(VkCommandBuffer command, bool rasterPrimary);
    VkRenderPass skyPass{};
    VkFramebuffer skyFramebuffer{};
    uint32_t queueFamily{};
    std::unique_ptr<denoise::Renderer> denoiser;
    VkPipeline temporalCompute{}, confidenceCompute{};
    VkPipelineLayout temporalLayout{};
    uint64_t shadowFrames = 0, shadowResets = 0;
    bool sigmaCapable = false, localCapable[2] = {};
    bool postSigma = false, postSigmaRequested = false;
    bool localSeen = false, postLocal = false, postLocalRequested = false;
    bool postLocalDenoise = false, postLocalDenoiseRequested = false;
    uint64_t previousLocalGeneration = 0;
    float previousLocalIntensity = 0, previousLocalRange = 0;
    uint32_t previousLocalActive = 0, previousLocalAdaptive = 0, localResponsiveFrames = 0;
    bool previousLocalBlueNoise = false;
    float previousSun[9] = {}, pendingSun[9] = {};
    std::unique_ptr<Buffer> blueNoise;
    bool blueNoiseFailed = false;
    void configureBlueNoise(VkCommandBuffer command, Frame &frame, const TraceOptions &options);
    bool postReblur = false, postTaa = false, postAttempted = false, rendered = false;
    std::string postFailure;
    remixapi_DenoisingCapabilitiesEXT postCapabilities{};
    scene::TemporalHistory history;
    std::map<scene::TemporalHistory::Key, std::shared_ptr<Buffer>> previousVertices, pendingVertices,
        retiredPreviousVertices;
    scene::Matrix previousCamera{}, pendingCamera{}, previousView{}, previousProjection{}, pendingProjection{};
    float previousJitter[2] = {};
    denoise::Frame temporalFrame{};
    uint32_t previousSeed{};
    std::chrono::steady_clock::time_point previousTime{}, pendingTime{};
    uint64_t historyResets = 0, denoisedFrames = 0, postPeakMemory = 0;
    std::vector<std::shared_ptr<Arena>> arenas;
    std::unique_ptr<Buffer> scratch, table, uniform;
    scene::LightBvh lightBvh;
    scene::EmissionHistory emissionHistory;
    scene::LightBvh torchBvh, emissiveBvh;
    std::unique_ptr<Buffer> torchNodes, torchEmitters, emissiveNodes, emissiveEmitters;
    uint64_t previousFamilyGeneration[2] = {};
    uint32_t familyResponsive[2] = {};
    bool splitLightingActive = false, postSplit = false, postSplitRequested = false;
    bool postCombinedDirect = false;
    std::unique_ptr<Buffer> lightNodes, lightEmitters;
    uint64_t lightBufferGeneration = 0, torchBufferGeneration = 0, emissiveBufferGeneration = 0;
    struct LightGuiding {
      static constexpr uint32_t capacity = 65536;
      std::unique_ptr<Buffer> history[2], readback;
      uint32_t writeIndex = 0;
      bool valid = false, pending = false, attempted = false, pipelineFailed = false;
      uint64_t generation = 0;
      float range = 0;
      bool resampled = false;
      bool split = false;
      remixapi_LocalLightGuidingStatisticsEXT stats{.structSize = sizeof(remixapi_LocalLightGuidingStatisticsEXT)};
    } lightGuiding;
    void configureLightGuiding(Frame &frame, const TraceOptions &options);
    void beginLightGuiding(VkCommandBuffer command);
    void finishLightGuiding(VkCommandBuffer command);
    void completeLightGuiding();
    std::vector<std::unique_ptr<Buffer>> uploads;
    std::vector<std::shared_ptr<Accel>> retired;
    struct GeometryGroup {
      uint32_t surface = 0, count = 0;
      VkDeviceSize indices = 0, triangleMap = 0;
      bool opaque = false;
    };
    struct Geometry {
      std::unique_ptr<Buffer> input;
      std::vector<VkAccelerationStructureGeometryKHR> geometries;
      std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
      std::vector<uint32_t> counts;
      std::vector<VkDeviceSize> indexOffsets;
      std::vector<GeometryGroup> groups;
      std::shared_ptr<Accel> accel;
      uint64_t fingerprint = 0, revision = 0;
      float diagonal = 0;
      std::array<float, 3> buildLow{}, buildHigh{};
      uint32_t refits = 0;
      bool deform = false, pending = true, update = false;
      VkAccelerationStructureBuildSizesInfoKHR sizes = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    };
    struct MeshView {
      std::weak_ptr<scene::Mesh> owner;
      std::shared_ptr<const scene::MeshUpdateStream> updateStream;
      std::shared_ptr<Geometry> geometry;
      std::vector<std::shared_ptr<Buffer>> vertices;
      std::vector<scene::Matrix> bones;
      uint64_t fingerprint = 0, lastUse = 0;
    };
    struct Tlas {
      std::shared_ptr<Accel> accel;
      std::unique_ptr<Buffer> instances;
      trace::TlasPolicy policy;
    } tlas[2];
    std::unordered_map<std::string, std::weak_ptr<Geometry>> geometryCache;
    scene::OpacityCache opacityCache;
    scene::SurfaceEmissionCache surfaceEmissionCache;
    using MeshViewKey = std::tuple<const scene::Mesh *, uint64_t, uint32_t, std::string>;
    std::map<MeshViewKey, MeshView> meshes;
    std::map<std::pair<const scene::MeshUpdateStream *, std::string>, MeshViewKey> updateViews;
    std::vector<std::shared_ptr<Geometry>> retiredGeometry;
    std::vector<std::shared_ptr<Buffer>> retiredBuffers;
    std::vector<std::shared_ptr<Geometry>> compactPending;
    remixapi_TracingStatisticsEXT stats = {.structSize = sizeof(remixapi_TracingStatisticsEXT)};
    struct Sharc {
      std::unique_ptr<Buffer> hash, accumulation, resolved, counters, readback;
      VkPipeline update{}, resolve{}, query{}, transparency{}, temporal{}, temporalTransparency{}, confidence{};
      VkQueryPool timestamps{};
      bool clear = true, pending = false, attempted = false, hasReceivers = false;
      scene::TemporalEpoch epoch{};
      uint64_t reset = 0, warmFrames = 0;
      uint32_t scale = 0, bounces = 0, responsiveFrames = 0, warmTarget = 64;
      uint32_t localConfig[4] = {};
      float localIntensity = 0, localRange = 0;
      float previousEye[3] = {}, light[9] = {};
      std::vector<uint8_t> geometry, dynamicGeometry;
      remixapi_SharcStatisticsEXT stats{.structSize = sizeof(remixapi_SharcStatisticsEXT)};
    } sharc;
    struct FrameResources {
      VkDescriptorSet set = VK_NULL_HANDLE;
      VkQueryPool timestamps = VK_NULL_HANDLE, compaction = VK_NULL_HANDLE,
                  sharcTimestamps = VK_NULL_HANDLE;
      bool timingPending = false, sharcPending = false, guidingPending = false, reusePending = false, rayPending = false;
      std::unique_ptr<Buffer> table, uniform, scratch, topScratch, instances[2];
      std::unique_ptr<Buffer> lightNodes, lightEmitters, torchNodes, torchEmitters, emissiveNodes,
          emissiveEmitters;
      std::unique_ptr<Buffer> sharcReadback, guidingReadback, reuseReadback, rayReadback;
      uint64_t lightGeneration = 0, torchGeneration = 0, emissiveGeneration = 0;
      std::vector<std::unique_ptr<Buffer>> uploads;
      std::vector<std::shared_ptr<Accel>> retired;
      std::vector<std::shared_ptr<Geometry>> compactPending, retiredGeometry;
      std::vector<std::shared_ptr<Buffer>> retiredBuffers;
      std::map<scene::TemporalHistory::Key, std::shared_ptr<Buffer>> retiredVertices;
    } frames[gpuFrameCount];
    uint32_t frameIndex = 0;
    void swapFrame(FrameResources &frame);
    void selectFrame(uint32_t slot);
    void beginSharc(const TraceOptions &options);
    void destroySharc(bool temporalOnly = false);
    void configureSharc(Frame &frame, const TraceOptions &options, scene::TemporalEpoch epoch,
                        std::vector<uint8_t> geometry, std::vector<uint8_t> dynamicGeometry, bool hasReceivers);
    void recordSharc(VkCommandBuffer command, const TraceOptions &options);
    void finishSharc(VkCommandBuffer command);
    void sharcPipeline(const uint32_t *code, size_t size, VkPipelineLayout pipelineLayout, VkPipeline &out);
    std::vector<uint8_t> lastScene;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    ~Gpu();
    std::shared_ptr<trace::detail::BufferCache> bufferCache =
        std::make_shared<trace::detail::BufferCache>();
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host = false,
                                   bool reusable = false);
    void write(Buffer &buffer, const void *data, size_t size);
    void hostBuffer(std::unique_ptr<Buffer> &out, const void *data, size_t size, VkBufferUsageFlags usage);
    std::unique_ptr<Buffer> upload(VkCommandBuffer command, const void *data, size_t size,
                                   VkBufferUsageFlags usage);
    std::shared_ptr<Accel> acceleration(VkDeviceSize size, VkAccelerationStructureTypeKHR type);
    VkBuildAccelerationStructureFlagsKHR flags(bool deform) const;
    MeshView &mesh(VkCommandBuffer command, const scene::Draw &draw,
                   const std::vector<std::shared_ptr<const scene::OpacityPartition>> &partitions);
    void complete(VkCommandBuffer command);
    void buildBottom(VkCommandBuffer command);
    std::unique_ptr<Buffer> topScratch;
    void buildTop(VkCommandBuffer command, const std::vector<trace::InstanceInput> inputs[2], bool force);
    std::unique_ptr<Image> image(bool colorAttachment = false,
                                 VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT, VkExtent2D size = {});
    void destroyPost();
    void beginFrame(const TraceOptions &options);
    void destroyTarget();
    void prepare(VkCommandBuffer command, RasterRenderer &resources, const TraceOptions &options,
                 scene::TraceFrame &scene);
  };
} // namespace remix
