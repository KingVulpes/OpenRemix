#pragma once
#include "pathtrace/renderer.h"
#include "pathtrace/tlas_policy.h"
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
  struct Buffer {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0;
    ~Buffer() {
      if (handle)
        vkDestroyBuffer(device, handle, nullptr);
      if (memory)
        vkFreeMemory(device, memory, nullptr);
    }
  };
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
    uint64_t previousVertices = 0, padding = 0;
    Matrix previousModel{};
  };
  static_assert(sizeof(TraceSurface) == 176 && sizeof(TraceVertex) == 48);
  struct Frame {
    Matrix inverseCamera, camera;
    float eye[4] = {}, direction[4] = {}, radiance[4] = {};
    uint32_t config[4] = {}, target[4] = {};
    float jitter[4] = {}; // primary sample offset in pixels; zw reserved
  };
  static_assert(sizeof(Frame) == 224);
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
    VkPipeline compute = VK_NULL_HANDLE, composite[2] = {};
    VkShaderModule shaders[3] = {};
    VkQueryPool compaction = VK_NULL_HANDLE, timestamps = VK_NULL_HANDLE;
    uint32_t timestampBits = 0;
    bool timingPending = false, imagesInitialized = false, encodeOutput = false;
    VkExtent2D extent = {};
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
    std::unique_ptr<Image> primary[5];
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
    bool postReblur = false, postTaa = false, postAttempted = false, rendered = false;
    std::string postFailure;
    remixapi_DenoisingCapabilitiesEXT postCapabilities{};
    scene::TemporalHistory history;
    std::map<scene::TemporalHistory::Key, std::shared_ptr<Buffer>> previousVertices, pendingVertices,
        retiredPreviousVertices;
    scene::Matrix previousCamera{}, previousView{}, previousProjection{}, pendingProjection{};
    float previousJitter[2] = {};
    denoise::Frame temporalFrame{};
    uint32_t previousSeed{};
    std::chrono::steady_clock::time_point previousTime{}, pendingTime{};
    uint64_t historyResets = 0, denoisedFrames = 0, postPeakMemory = 0;
    std::vector<std::shared_ptr<Arena>> arenas;
    std::unique_ptr<Buffer> scratch, table, uniform;
    std::vector<std::unique_ptr<Buffer>> uploads;
    std::vector<std::shared_ptr<Accel>> retired;
    struct Geometry {
      std::unique_ptr<Buffer> input;
      std::vector<VkAccelerationStructureGeometryKHR> geometries;
      std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
      std::vector<uint32_t> counts;
      std::vector<VkDeviceSize> indexOffsets;
      std::shared_ptr<Accel> accel;
      uint64_t fingerprint = 0, revision = 0;
      float diagonal = 0;
      bool deform = false, pending = true, update = false;
      VkAccelerationStructureBuildSizesInfoKHR sizes = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    };
    struct MeshView {
      std::weak_ptr<scene::Mesh> owner;
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
    std::map<std::tuple<const scene::Mesh *, uint64_t, uint32_t>, MeshView> meshes;
    std::vector<std::shared_ptr<Geometry>> compactPending;
    remixapi_TracingStatisticsEXT stats = {.structSize = sizeof(remixapi_TracingStatisticsEXT)};
    std::vector<uint8_t> lastScene;
    uint64_t sequence = 0;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    ~Gpu();
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> buffer(VkDeviceSize size, VkBufferUsageFlags usage, bool host = false);
    void write(Buffer &buffer, const void *data, size_t size);
    void hostBuffer(std::unique_ptr<Buffer> &out, const void *data, size_t size, VkBufferUsageFlags usage);
    std::unique_ptr<Buffer> upload(VkCommandBuffer command, const void *data, size_t size,
                                   VkBufferUsageFlags usage);
    std::shared_ptr<Accel> acceleration(VkDeviceSize size, VkAccelerationStructureTypeKHR type);
    VkBuildAccelerationStructureFlagsKHR flags(bool deform) const;
    MeshView &mesh(VkCommandBuffer command, const scene::Draw &draw);
    void complete(VkCommandBuffer command);
    void buildBottom(VkCommandBuffer command);
    std::unique_ptr<Buffer> topScratch;
    void buildTop(VkCommandBuffer command, const std::vector<trace::InstanceInput> inputs[2], bool force);
    std::unique_ptr<Image> image(bool colorAttachment = false,
                                 VkFormat format = VK_FORMAT_R32G32B32A32_SFLOAT);
    void destroyPost();
    void beginFrame(const TraceOptions &options);
    void destroyTarget();
    void prepare(VkCommandBuffer command, RasterRenderer &resources, const TraceOptions &options);
  };
} // namespace remix
