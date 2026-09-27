#pragma once
#include "pathtrace/buffer_cache.h"
#include "pathtrace/frame_tables.h"
#include "pathtrace/renderer.h"
#include "pathtrace/tlas_policy.h"
#include "platform/gpu_timers.h"
#include "platform/timing.h"
#include "platform/workers.h"
#include "scene/emission.h"
#include "scene/light_bvh.h"
#include "scene/opacity.h"
#include "scene/static_scene.h"
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
  // TraceSurface: pathtrace/frame_tables.h.
  static_assert(sizeof(TraceVertex) == 48);
  struct Frame {
    Matrix inverseCamera, camera;
    float eye[4] = {}, direction[4] = {}, radiance[4] = {};
    uint32_t config[4] = {}, target[4] = {};
    float jitter[4] = {};      // primary offset; z water reuse, w blue-noise sun/local bits
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
    float firefly[4] = {};       // outgoing local direct / indirect luminance limits; reserved
    uint32_t pathConfig[4] = {}; // adaptive termination, minimum completed bounces, reserved
    float domeTransform[3][4] = {};
  };
  static_assert(sizeof(Frame) == 592 && offsetof(Frame, pathConfig) == 528 &&
                offsetof(Frame, firefly) == 512 && offsetof(Frame, indirectResults) == 496 &&
                offsetof(Frame, lightGuideRead) == 384 && offsetof(Frame, lightGuideConfig) == 400 &&
                offsetof(Frame, lightNodes) == 336 && offsetof(Frame, localConfig) == 352 &&
                offsetof(Frame, localScale) == 368 && offsetof(Frame, skyRadiance) == 224 &&
                offsetof(Frame, sharcHash) == 240 && offsetof(Frame, sharcConfig) == 272 &&
                offsetof(Frame, sharcGrid) == 288 && offsetof(Frame, sharcPreviousCamera) == 304 &&
                offsetof(Frame, sun) == 320);
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
    remixapi_FrameBreakdownEXT breakdown{.structSize = sizeof(remixapi_FrameBreakdownEXT)};
    uint64_t materialTicks = 0, temporalTicks = 0;
    // REMIX_FRAME_DIGEST: address-free hash of the prepared GPU inputs.
    const bool digestEnabled = std::getenv("REMIX_FRAME_DIGEST") != nullptr;
    uint64_t digest = 0;
    std::vector<std::shared_ptr<trace::detail::BufferMemory>> bufferMemory;
    using Buffer = trace::detail::Buffer;
    using Image = trace::detail::Image;
    using Arena = trace::detail::Arena;
    using Accel = trace::detail::Accel;
    using TraceVertex = trace::detail::TraceVertex;
    using TraceSurface = trace::detail::TraceSurface;
    using Frame = trace::detail::Frame;

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
    VkPipeline transparencyCompute{}, temporalTransparencyCompute{};
    VkPipeline singleTransparencyCompute{}, singleTemporalTransparencyCompute{}, frontGuidesCompute{};
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
      const char *modeFile = std::getenv("REMIX_INDIRECT_MODE_FILE");
      uint64_t controlReset = UINT64_MAX;
    } indirectPass;
    void configureIndirect(Frame &frame, const TraceOptions &options, bool querySharc);
    void configureProfile(const TraceOptions &options);
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
      bool supportsRoulette = true;
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
    bool decalMaterialsActive = false;
    bool localIlluminationActive = false;
    bool localResamplingActive = false;
    bool localAngularActive = false;
    bool localGuidingActive = false;
    bool rouletteActive = false, rouletteFailed = false;
    VkPipeline selectLightingPipeline(VkPipeline pipeline, bool combinedDirect = false,
                                      bool separateIndirect = false);
    void forgetLightingPipeline(VkPipeline pipeline);
    VkShaderModule shaders[3] = {};
    VkQueryPool compaction = VK_NULL_HANDLE, timestamps = VK_NULL_HANDLE;
    uint32_t timestampBits = 0;
    const char *stageProfile = std::getenv("REMIX_STACK_PROFILE");
    const char *stateProfile = stageProfile ? std::getenv("REMIX_TRACE_STATE") : nullptr;
    uint32_t selectedLightingKey = 0;
    std::string traceState;
    // Opt-in placement experiment: stage frequently-read scene tables into VRAM.
    bool deviceLocalScene = [] {
      const auto *value = std::getenv("REMIX_DEVICE_LOCAL_SCENE");
      return value && std::strcmp(value, "1") == 0;
    }();
    std::unique_ptr<Buffer> tableUpload;
    // Diagnostic replay must measure fresh lighting even after a static view
    // converges. Keep camera/history/cache contents intact and only bypass the
    // final-image reuse shortcut. Ordinary rendering retains that shortcut.
    const bool profileEveryFrame = [] {
      const char *value = std::getenv("REMIX_PROFILE_EVERY_FRAME");
      return value && std::strcmp(value, "1") == 0;
    }();
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
    uint32_t reconstructionWarmFrames = 0;
    bool rrActive() const { return upscaler && upscaler->statistics().backend == upscale::Backend::DlssRr; }
    TraceOptions effectiveOptions(TraceOptions options) const;
    upscale::Backend selectBackend() const;
    uint32_t fsrQuality = 1;
    std::unique_ptr<upscale::Renderer> upscaler;
    std::string fsrFailure;
    void createTarget();
    std::unique_ptr<Image> world, transmission, sky;
    std::unique_ptr<postfx::Renderer> postFallback;
    postfx::Renderer *postProcessing = nullptr;
    bool retainPostExposure = false;
    VkDescriptorSetLayout postSetLayout{};
    using PrimaryDraw = trace::PrimaryDraw;
    struct PrimaryPush {
      scene::Matrix model;
      float jitter[2] = {};
      uint32_t surfaceIndex = 0, stableDepth = 0;
      std::array<float, 4> reverseDepth{};
    };
    static_assert(sizeof(PrimaryPush) == 96);
    std::array<float, 4> primaryReverseDepth{};
    // The frame's primary draws (the frame tables' static prefix, then the rest).
    size_t primaryCount() const { return tables.primaryCount(); }
    const PrimaryDraw &primaryDraw(size_t i) const { return tables.primary(i); }
    std::unique_ptr<Image> primary[7];
    VkRenderPass primaryPass[2] = {};
    VkFramebuffer primaryFramebuffer[2] = {};
    VkPipeline primaryPipeline[2] = {};
    VkPipelineLayout primaryLayout{};
    bool primaryInitialized = false;
    void createPrimary(VkFormat depthFormat, VkImageView depthView);
    void destroyPrimary();
    void recordPrimary(VkCommandBuffer command, bool rasterPrimary);
    // Large primary passes record secondary command buffers on the workers:
    // one transient pool per task and frame slot, reset after the slot's fence.
    struct PrimaryRecording {
      std::vector<VkCommandPool> pools;
      std::vector<VkCommandBuffer> buffers; // two per pool: primary passes 0 and 1
      bool reset = false;                   // pools reset for this frame
    } primaryRecording;
    void destroyPrimaryRecording(PrimaryRecording &recording);
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
    using VertexHistory = scene::TemporalTable<std::shared_ptr<Buffer>>;
    VertexHistory previousVertices, pendingVertices, retiredPreviousVertices;
    scene::Matrix previousCamera{}, pendingCamera{}, previousView{}, previousProjection{},
        pendingProjection{};
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
      remixapi_LocalLightGuidingStatisticsEXT stats{.structSize =
                                                        sizeof(remixapi_LocalLightGuidingStatisticsEXT)};
    } lightGuiding;
    void configureLightGuiding(Frame &frame, const TraceOptions &options);
    void beginLightGuiding(VkCommandBuffer command);
    void finishLightGuiding(VkCommandBuffer command);
    void completeLightGuiding();
    std::vector<std::unique_ptr<Buffer>> uploads;
    // One mapped staging buffer per frame slot, filled front to back by
    // upload(). Reset once the slot's fence has completed (complete()).
    struct Staging {
      std::unique_ptr<Buffer> buffer;
      VkDeviceSize used = 0;
    } staging;
    VkDeviceSize stage(const void *data, size_t size);
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
      uint64_t contentDigest = 0; // REMIX_FRAME_DIGEST only: hash of the uploaded data.
    };
    // CPU half of a mesh view conversion: skinning, BLAS input assembly and
    // content hashes. Independent per view, so conversions run on workers.
    struct GeometryScratch {
      std::string content;
      std::vector<std::vector<TraceVertex>> shading;
      std::vector<VkDeviceSize> vertexOffsets, indexOffsets;
      std::vector<uint32_t> counts, vertexCounts;
      std::vector<GeometryGroup> groups;
      std::vector<uint64_t> shadingHashes; // scene::fastHash of each shading buffer
      uint64_t contentHash = 0;            // scene::fastHash of content
      float low[3] = {}, high[3] = {};
      size_t bytes() const {
        size_t size = content.capacity() + shading.capacity() * sizeof(std::vector<TraceVertex>) +
                      (vertexOffsets.capacity() + indexOffsets.capacity()) * sizeof(VkDeviceSize) +
                      (counts.capacity() + vertexCounts.capacity()) * sizeof(uint32_t) +
                      groups.capacity() * sizeof(GeometryGroup) + shadingHashes.capacity() * sizeof(uint64_t);
        for (const auto &vertices : shading)
          size += vertices.capacity() * sizeof(TraceVertex);
        return size;
      }
    };
    // A view that needs new geometry this frame (meshView), between its CPU
    // conversion (convertMesh, any thread) and its GPU upload (commitMesh).
    struct MeshConversion {
      MeshView *view = nullptr;
      const scene::Draw *draw = nullptr;
      const scene::OpacityPartition *const *partitions = nullptr;
      std::string opaque;
      bool skinned = false, streaming = false, deform = false;
      std::shared_ptr<Geometry> previousGeometry;
      GeometryScratch data;
    };
    // Finds the draw's view. When it needs new geometry, prepares conversion
    // (view state updated as the serial path does) and returns true.
    MeshView &meshView(const scene::Draw &draw, const scene::OpacityPartition *const *partitions,
                       size_t partitionCount, MeshConversion &conversion, bool &convert);
    struct InstanceView {
      MeshView *view = nullptr;
      int32_t conversion = -1; // index into conversions, or -1
    };
    std::vector<InstanceView> instanceViews;
    std::vector<const scene::OpacityPartition *> partitionStorage;
    // Read-only: the view meshView would return unchanged for a draw that is
    // not streamed (no lineage transfer) and needs no conversion, else null.
    // Safe to run concurrently; marks the view used this frame.
    MeshView *findView(const scene::Draw &draw, const scene::OpacityPartition *const *partitions,
                       size_t partitionCount);
    std::vector<size_t> partitionOffsets;
    static void convertMesh(MeshConversion &conversion);
    void commitMesh(VkCommandBuffer command, MeshConversion &conversion);
    std::vector<MeshConversion> conversions; // reused storage; see Gpu::prepare
    std::unique_ptr<WorkerPool> workers;
    struct Tlas {
      std::shared_ptr<Accel> accel;
      std::unique_ptr<Buffer> instances;
      trace::TlasPolicy policy;
      // Combined TLAS only: the combinedStaticStamp whose static instances
      // this slot's instance buffer already holds (0: none).
      uint64_t prefix = 0;
    } tlas[3]; // static, dynamic, combined (rtx.native.pathtrace.combinedTlas)
    // Combined TLAS: its policy and the static part's instance records, kept
    // while the static inputs do not change.
    trace::CombinedTlasPolicy combinedPolicy;
    std::vector<VkAccelerationStructureInstanceKHR> combinedStatic, combinedDynamic;
    uint64_t combinedStaticStamp = 0;
    bool combinedStaticValid = false;
    void buildCombinedTop(VkCommandBuffer command, const std::vector<trace::InstanceInput> &staticInputs,
                          const std::vector<trace::InstanceInput> &dynamicInputs, bool force,
                          uint64_t staticGeneration);
    struct ContentHash {
      size_t operator()(const std::string &content) const {
        return size_t(scene::fastHash(content.data(), content.size()));
      }
    };
    std::unordered_map<std::string, std::weak_ptr<Geometry>, ContentHash> geometryCache;
    // Persistent static set ("rtx.native.scene.persistentStatic"): the units
    // (with their temporal history) and the frame tables that keep their
    // surface table entries across frames. The Env of the tables: the
    // tracer's mesh views.
    struct TableViews {
      using View = MeshView;
      using VertexTable = scene::TemporalTable<std::shared_ptr<trace::detail::Buffer>>;
      using Span = scene::TraceFrame::InstanceList::ConstSpan;
      PathTracer::Gpu *gpu = nullptr;
      VkCommandBuffer command = VK_NULL_HANDLE;
      void resolveViews(const std::vector<Span> &instances, std::vector<const MeshView *> &views);
      bool findViews(const std::vector<Span> &instances, std::vector<const MeshView *> &views);
      std::shared_ptr<const void> owner(const MeshView &view, uint32_t surface) const {
        return view.vertices.at(surface);
      }
      [[noreturn]] void tooManyObjects() const { throw VK_ERROR_TOO_MANY_OBJECTS; }
    };
    scene::StaticScene statics;
    trace::FrameTables<TableViews> tables;
    std::vector<std::shared_ptr<const void>> retiredStates;
    uint64_t prepareSerial = 0, renderedSerial = 0;
    // The frame tables' prefix version this slot's surface table holds.
    uint64_t tableVersion = 0;
    // The static prefix's texture descriptors, resolved once per prefix.
    std::vector<VkDescriptorImageInfo> prefixTextures;
    uint64_t prefixTexturesGeneration = 0;
    bool staticValidation = false, staticMismatchLogged = false;
    PathTracer::StaticStatistics staticStatistics;
    scene::OpacityCache opacityCache;
    scene::SurfaceEmissionCache surfaceEmissionCache;
    using MeshViewKey = std::tuple<const scene::Mesh *, uint64_t, uint32_t, std::string>;
    struct MeshViewKeyHash {
      size_t operator()(const MeshViewKey &key) const {
        const auto &opaque = std::get<3>(key);
        const uint64_t values[] = {uint64_t(reinterpret_cast<uintptr_t>(std::get<0>(key))), std::get<1>(key),
                                   std::get<2>(key), scene::fastHash(opaque.data(), opaque.size())};
        return size_t(scene::fastHash(values, sizeof(values)));
      }
    };
    // Looked up once per traced instance per frame; iteration order is unused
    // beyond collecting BLAS builds, whose results do not depend on order.
    std::unordered_map<MeshViewKey, MeshView, MeshViewKeyHash> meshes;
    // Update lineage -> its current view. Keyed like MeshViewKey with the
    // stream in place of the mesh: skinned lineages keep one view per instance.
    using UpdateViewKey = std::tuple<const scene::MeshUpdateStream *, uint64_t, uint32_t, std::string>;
    std::map<UpdateViewKey, MeshViewKey> updateViews;
    // Views of per-instance skins (keys with an instance), which retire when
    // unused; rigid views retire only when their CPU mesh is released, so
    // retirement scans them only after scene::meshReleases changed.
    std::unordered_set<const MeshViewKey *> deformViews;
    uint64_t meshReleasesSeen = ~0ull;
    std::vector<std::shared_ptr<Geometry>> retiredGeometry;
    std::vector<std::shared_ptr<Buffer>> retiredBuffers;
    std::vector<std::shared_ptr<Geometry>> compactPending;
    remixapi_TracingStatisticsEXT stats = {.structSize = sizeof(remixapi_TracingStatisticsEXT)};
    struct Sharc {
      std::unique_ptr<Buffer> hash, accumulation, resolved, counters, readback;
      VkPipeline update{}, resolve{}, query{}, transparency{}, singleTransparency{}, temporal{}, temporalTransparency{}, singleTemporalTransparency{},
          confidence{};
      VkQueryPool timestamps{};
      bool clear = true, pending = false, attempted = false, hasReceivers = false;
      scene::TemporalEpoch epoch{};
      uint64_t reset = 0, warmFrames = 0;
      uint32_t scale = 0, bounces = 0, responsiveFrames = 0, warmTarget = 64;
      uint32_t localConfig[4] = {};
      float localIntensity = 0, localRange = 0;
      float previousEye[3] = {}, light[9] = {};
      std::vector<uint8_t> geometry, dynamicGeometry;
      uint64_t geometryGeneration = 0; // the frame tables' static prefix geometry holds
      remixapi_SharcStatisticsEXT stats{.structSize = sizeof(remixapi_SharcStatisticsEXT)};
    } sharc;
    struct LinearCapture {
      std::unique_ptr<Buffer> readback;
      std::string path;
      VkExtent2D extent{};
      bool packedPbr = false, hitDistance = false;
    } linearCapture;
    void captureLinear(VkCommandBuffer command);
    void completeLinearCapture();
    struct FrameResources {
      std::string traceState;
      LinearCapture linearCapture;
      VkDescriptorSet set = VK_NULL_HANDLE;
      VkQueryPool timestamps = VK_NULL_HANDLE, compaction = VK_NULL_HANDLE, sharcTimestamps = VK_NULL_HANDLE;
      bool timingPending = false, sharcPending = false, guidingPending = false, reusePending = false,
           rayPending = false;
      std::unique_ptr<Buffer> table, uniform, scratch, topScratch, instances[3];
      uint64_t combinedPrefix = 0; // Tlas::prefix of the combined TLAS's instances
      std::unique_ptr<Buffer> tableUpload;
      std::unique_ptr<Buffer> lightNodes, lightEmitters, torchNodes, torchEmitters, emissiveNodes,
          emissiveEmitters;
      std::unique_ptr<Buffer> sharcReadback, guidingReadback, reuseReadback, rayReadback;
      uint64_t lightGeneration = 0, torchGeneration = 0, emissiveGeneration = 0;
      std::vector<std::unique_ptr<Buffer>> uploads;
      Staging staging;
      PrimaryRecording primaryRecording;
      std::vector<std::shared_ptr<Accel>> retired;
      std::vector<std::shared_ptr<Geometry>> compactPending, retiredGeometry;
      std::vector<std::shared_ptr<Buffer>> retiredBuffers;
      scene::TemporalTable<std::shared_ptr<Buffer>> retiredVertices;
      std::vector<std::shared_ptr<const void>> retiredStates;
      uint64_t tableVersion = 0;
    } frames[gpuFrameCount];
    uint32_t frameIndex = 0;
    void swapFrame(FrameResources &frame);
    void selectFrame(uint32_t slot);
    void beginSharc(const TraceOptions &options);
    void destroySharc(bool temporalOnly = false);
    // geometryGeneration: nonzero when geometry is the frame tables' static
    // prefix of that generation (equal generations, equal bytes).
    void configureSharc(Frame &frame, const TraceOptions &options, scene::TemporalEpoch epoch,
                        const std::vector<uint8_t> &geometry, uint64_t geometryGeneration,
                        const std::vector<uint8_t> &dynamicGeometry, bool hasReceivers);
    void recordSharc(VkCommandBuffer command, const TraceOptions &options);
    void finishSharc(VkCommandBuffer command);
    void sharcPipeline(const uint32_t *code, size_t size, VkPipelineLayout pipelineLayout, VkPipeline &out);
    trace::SceneSignature lastScene;
    uint64_t sequence = 0;
    float previousDirectClamp = 0, previousIndirectClamp = 0;
    bool previousFireflyFilter = false;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    bool preparing = false, preparedRasterPrimary = true;
    bool preparedPerspective = false, preparedLocal = false;
    void createRawPipelines();
    void configureFrame(const TraceOptions &options, bool perspective,
                        bool local);
    ~Gpu();
    std::shared_ptr<trace::detail::BufferCache> bufferCache = std::make_shared<trace::detail::BufferCache>();
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host = false,
                                   bool reusable = false);
    void write(Buffer &buffer, const void *data, size_t size);
    // Writes consecutive parts; offset skips bytes the buffer already holds.
    void write(Buffer &buffer, std::initializer_list<std::pair<const void *, size_t>> parts, size_t offset = 0);
    void hostBuffer(std::unique_ptr<Buffer> &out, const void *data, size_t size, VkBufferUsageFlags usage);
    std::unique_ptr<Buffer> upload(VkCommandBuffer command, const void *data, size_t size,
                                   VkBufferUsageFlags usage);
    std::shared_ptr<Accel> acceleration(VkDeviceSize size, VkAccelerationStructureTypeKHR type);
    VkBuildAccelerationStructureFlagsKHR flags(bool deform) const;
    MeshView &mesh(VkCommandBuffer command, const scene::Draw &draw,
                   const std::vector<const scene::OpacityPartition *> &partitions);
    uint64_t geometryTicks = 0; // FramePerformanceEXT geometry time, per-instance timer.
    void complete(VkCommandBuffer command);
    void buildBottom(VkCommandBuffer command);
    std::unique_ptr<Buffer> topScratch;
    // staticGeneration: nonzero when staticInputs are the frame tables' static
    // prefix of that generation; one the static TLAS was built from (or found
    // equal to) is not compared again.
    // faceCulling: rtx.native.pathtrace.hardwareFaceCulling (trace::instanceFlags).
    // combined: rtx.native.pathtrace.combinedTlas; builds only tlas[2].
    void buildTop(VkCommandBuffer command, const std::vector<trace::InstanceInput> &staticInputs,
                  const std::vector<trace::InstanceInput> &dynamicInputs, bool force, uint64_t staticGeneration,
                  bool faceCulling, bool combined);
    uint64_t tlasStaticGeneration = 0;
    bool topFaceCulling = false; // the setting the TLAS instance flags were built with
    bool topCombined = false;    // the TLAS layout last built
    std::unique_ptr<Image> image(bool colorAttachment = false,
                                 VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT, VkExtent2D size = {});
    void destroyPost();
    void beginFrame(const TraceOptions &options);
    void destroyTarget();
    void prepare(VkCommandBuffer command, RasterRenderer &resources, const TraceOptions &options,
                 scene::TraceFrame &scene);
  };
} // namespace remix
