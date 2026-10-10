#pragma once
#include "raster/renderer.h"
#include "platform/timing.h"
#include "runtime/pipelines.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <utility>

namespace remix::raster::detail {
  using namespace scene;

  inline void check(VkResult result) {
    if (result != VK_SUCCESS) {
      throw result;
    }
  }
  struct Buffer {
    VkDevice device;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    size_t capacity = 0;
    VkDeviceSize allocationSize = 0;
    VkBufferUsageFlags usage = 0;
    VkMemoryPropertyFlags flags = 0;
    ~Buffer() {
      if (buffer) {
        vkDestroyBuffer(device, buffer, nullptr);
      }
      if (memory) {
        vkFreeMemory(device, memory, nullptr);
      }
    }
  };
  struct Image {
    VkDevice device;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t levels = 0;
    VkComponentMapping components = {};
    VkDeviceSize bytes = 0; // Device allocation size, for VRAM statistics.
    uint32_t width = 0, height = 0;
    VkImageCreateFlags flags = 0;
    // UNORM view of an sRGB (mutable-format) image: data maps alias it.
    VkImageView linearView = VK_NULL_HANDLE;
    ~Image() {
      if (linearView) {
        vkDestroyImageView(device, linearView, nullptr);
      }
      if (view) {
        vkDestroyImageView(device, view, nullptr);
      }
      if (image) {
        vkDestroyImage(device, image, nullptr);
      }
      if (memory) {
        vkFreeMemory(device, memory, nullptr);
      }
    }
  };
  struct Push {
    Matrix mvp;
    float tint[4];
    float uv[4] = {1, 1, 0, 0};
    uint32_t state[4] = {};
    float factor[4] = {};
  };
  static_assert(sizeof(Push) == 128);

  inline float linear(float c) { return c <= .04045f ? c / 12.92f : std::pow((c + .055f) / 1.055f, 2.4f); }
  template <typename Map, typename Predicate> void prune(Map &map, Predicate predicate) {
    for (auto it = map.begin(); it != map.end();) {
      if (predicate(*it)) {
        it = map.erase(it);
      } else {
        ++it;
      }
    }
  }
} // namespace remix::raster::detail

namespace remix {
  struct RasterRenderer::Gpu {
    using Buffer = raster::detail::Buffer;
    using Image = raster::detail::Image;
    using Push = raster::detail::Push;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memory = {};
    VkPhysicalDeviceProperties properties = {};
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    VkRenderPass pass = VK_NULL_HANDLE, skyPass = VK_NULL_HANDLE;
    VkExtent2D extent = {};
    bool encodeOutput = false, linearTarget = false;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    std::shared_ptr<Image> depth;
    struct MeshGpu {
      std::weak_ptr<scene::Surface> owner;
      std::unique_ptr<Buffer> data;
      VkDeviceSize indexOffset;
      VkDeviceSize spriteOffset = 0; // scene::Surface::sprite, after the indices (when it has any)
      // Object-space bounds of the vertices, computed on first halo use.
      bool bounded = false;
      float low[3] = {}, high[3] = {};
    };
    struct TextureGpu {
      std::weak_ptr<scene::Texture> owner;
      std::unique_ptr<Image> data; // Null for a linear alias of another entry's image.
      VkImageView view = VK_NULL_HANDLE; // Sampled view: data->view or the alias.
      uint64_t revision = 0;             // scene::TextureUpdates::revision uploaded
      bool updatable = false;            // owner has TextureUpdates: refreshed by prepare()
    };
    struct MaterialGpu {
      VkDevice device;
      VkDescriptorPool pool;
      std::weak_ptr<scene::Material> owner;
      VkSampler sampler = VK_NULL_HANDLE;
      VkDescriptorSet set = VK_NULL_HANDLE;
      ~MaterialGpu() {
        if (set) {
          vkFreeDescriptorSets(device, pool, 1, &set);
        }
        if (sampler) {
          pipeline::destroySampler(device, sampler, nullptr);
        }
      }
    };
    struct Command {
      Push push;
      VkPipeline pipeline;
      VkDescriptorSet set;
      VkBuffer vertices, indices;
      VkDeviceSize vertexOffset, indexOffset;
      uint32_t firstIndex, count;
      int32_t baseVertex;
      int layer;
      // A masked overlay (raster/mask.cpp): the group's coverage, bound as set 1
      // of mask.layout, and the pixels the draw is confined to.
      VkDescriptorSet coverage;
      VkRect2D area;
      // A sprite overlay (raster/sprite.cpp): the mesh's sprite vertices as
      // binding 1 (same buffer as its indices), and its parameters, which
      // replace push.factor (1 / softDistance, fade start, fade end, modulate k).
      bool sprite = false;
      VkBuffer spriteBuffer = VK_NULL_HANDLE;
      VkDeviceSize spriteOffset = 0;
      float spriteParameters[4] = {};
    };
    std::unordered_map<const scene::Surface *, MeshGpu> meshes;
    std::unordered_map<const scene::Texture *, TextureGpu> textures;
    std::unordered_map<const scene::Material *, std::unique_ptr<MaterialGpu>> materials;
    std::unordered_map<uint64_t, VkPipeline> pipelines;
    std::vector<std::unique_ptr<Buffer>> uploads;
    std::vector<std::unique_ptr<Buffer>> reusableBuffers;
    VkDeviceSize reusableBytes = 0;
    void recycle(std::unique_ptr<Buffer> buffer);
    std::unique_ptr<Buffer> skin;
    struct FrameResources {
      std::vector<std::unique_ptr<Buffer>> uploads, retiredBuffers;
      std::vector<std::unique_ptr<Image>> retiredImages;
      std::vector<std::unique_ptr<MaterialGpu>> retiredMaterials;
      std::unique_ptr<Buffer> skin;
    } frames[gpuFrameCount];
    uint32_t frameIndex = 0;
    uint64_t uploadBytes = 0; // Staged by buffer(), for RasterRenderer::profile().
    // Counts the uploads that changed a dynamic texture's image
    // (remixapi_UpdateTextureEXT): what the images show, for the traced
    // scene's signature.
    uint64_t dynamicTextureUploads = 0;
    std::vector<Command> commands;
    // Halo outlines (remixapi_InstanceInfoHaloEXT, raster/halo.cpp). Nothing
    // here exists until a frame submits a halo instance; the objects are
    // destroyed with the target.
    struct Halo {
      struct Target {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
      };
      // Coverage at target resolution: the silhouette mask and two blur targets.
      Target targets[3];
      // loadPass: maskPass keeping the target's contents. earlyPass,
      // earlyLoadPass: the same two before the frame has a depth (the depth
      // attachment is neither loaded nor stored; untested silhouettes only).
      VkRenderPass maskPass = VK_NULL_HANDLE, loadPass = VK_NULL_HANDLE, blurPass = VK_NULL_HANDLE;
      VkRenderPass earlyPass = VK_NULL_HANDLE, earlyLoadPass = VK_NULL_HANDLE;
      VkPipelineLayout layout = VK_NULL_HANDLE; // blur and composite
      VkShaderModule maskFragment = VK_NULL_HANDLE, vertex = VK_NULL_HANDLE, blurFragment = VK_NULL_HANDLE,
                     compositeFragment = VK_NULL_HANDLE;
      VkSampler sampler = VK_NULL_HANDLE;
      // mask: [depth tested]; composite: [additive].
      VkPipeline mask[2] = {}, blur = VK_NULL_HANDLE, composite[2] = {};
      bool ready = false;  // every pipeline is compiled
      bool coverageReady = false; // the silhouette passes and mask pipelines (prepareCoverage)
      bool failed = false; // creation failed: no halos until the target changes
      struct Group {
        scene::Halo halo;
        uint32_t first = 0, count = 0; // in commands
        VkRect2D area = {};            // pixels the group can change
        bool bounded = false;
        float low[2] = {}, high[2] = {}; // silhouette bounds while accumulating
      };
      std::vector<Group> groups; // this frame's, in composite order
      std::vector<Command> commands;
    } halo;
    // Requests the halo pipelines (creating the passes they need); false while
    // they compile.
    bool prepareHalo();
    void createHaloTargets();
    void destroyHaloTargets();
    // Sets the group's pixel area from its silhouette bounds and blur reach;
    // false when it reaches no pixel.
    bool haloArea(Halo::Group &group) const;
    void destroyHalo();
    // Silhouette coverage, shared by halo outlines and masked overlays: the
    // sampler, the silhouette passes (halo.maskPass, loadPass) and the
    // silhouette pipelines (halo.mask). False while they compile.
    bool prepareCoverage();
    // A silhouette pipeline for halo.maskPass: the raster vertex stage, the
    // given fragment stage and layout, both faces, optionally depth-tested
    // (biased) against the frame's depth. False while it compiles.
    bool coveragePipeline(VkPipeline &out, VkShaderModule fragmentModule, VkPipelineLayout pipelineLayout,
                          bool tested);
    // An R8 target at target resolution with its framebuffer (tested: with the
    // frame's depth attachment, for halo.maskPass/loadPass) and sampling set.
    void createCoverageTarget(Halo::Target &target, bool tested);
    void destroyCoverageTarget(Halo::Target &target);
    // Sets the group's pixel area from its silhouette bounds grown by margin
    // (pixels, x and y); false when it reaches no pixel.
    bool coverageArea(Halo::Group &group, const float *margin) const;
    // Masked overlays (remixapi_InstanceInfoMaskEXT and masked UI commands,
    // raster/mask.cpp). As for halos, nothing exists until a frame uses a mask.
    struct Mask {
      // One coverage target per group that has sources in a frame, in the order
      // the groups first appear; kept until the target changes.
      std::vector<Halo::Target> targets;
      VkPipelineLayout layout = VK_NULL_HANDLE; // the raster layout and, set 1, a coverage target
      VkShaderModule withinFragment = VK_NULL_HANDLE, overlayFragment = VK_NULL_HANDLE;
      VkPipeline within[2] = {}; // sources limited to another group: [depth tested]
      bool ready = false, failed = false;
      // This frame's coverage passes, in order: sources with equal parameters.
      struct Run {
        uint32_t target = 0;
        int32_t within = -1;           // the target limiting these sources
        bool clear = false;            // the target's first run
        uint32_t first = 0, count = 0; // in commands
        bool early = false;            // also drawn before the scene is traced (cut)
      };
      std::vector<Run> runs;
      std::vector<Command> commands;
      int32_t slot[scene::Mask::maxGroup + 1] = {}; // group -> target this frame, -1: no sources
      VkRect2D area[scene::Mask::maxGroup] = {};    // per target: pixels its coverage can be set in
      uint32_t used = 0;                            // targets holding this frame's coverage
      // Commands [0, split) are recorded before the coverage passes: every
      // world-layer draw that is not masked.
      size_t split = 0;
      // Role CUT with a traced scene: the path tracer resolves primary
      // visibility long before the coverage passes, so the runs of the groups
      // that cut (and of the groups those are `within`) are marked early and
      // drawn once more ahead of it, into the same targets, without the depth
      // test (there is no depth yet). The tracer samples the targets; the
      // coverage passes then draw every run again for the masked overlays.
      struct Cut {
        uint32_t count = 0;                             // groups that cut this frame
        uint32_t targets[scene::Mask::maxCutGroups] = {}; // their targets, in slot order
        // Four bits per group (1..32): its slot + 1, or 0 when it does not cut.
        uint32_t slots[4] = {};
        uint64_t digest = 0; // changes with the early sources
      } cut;
    } mask;
    // Sprite overlays (remixapi_InstanceInfoSpriteEXT, raster/sprite.cpp).
    struct Sprite {
      VkPipelineLayout layout = VK_NULL_HANDLE; // the raster layout and, set 1, the scene's depth
      VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
      std::unique_ptr<Image> depth; // copy of the target's depth attachment
      VkSampler sampler = VK_NULL_HANDLE;
      VkDescriptorSet set = VK_NULL_HANDLE;
      bool failed = false;
      uint32_t soft = 0;     // this frame's commands with SOFT_DEPTH
      bool captured = false; // this frame's depth is in the copy
    } sprite;
    // The layout and shader modules; false when they cannot be created.
    bool prepareSprite();
    void destroySprite();
    bool prepareMask();
    // The coverage passes: every run, or (early) the runs marked early.
    void recordMaskRuns(VkCommandBuffer command, bool early);
    void createMaskTargets(uint32_t count);
    void destroyMask();
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    ~Gpu();
    void destroyTarget();
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> allocateBuffer(size_t size, VkBufferUsageFlags usage,
                                           VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> buffer(const void *data, size_t size, VkBufferUsageFlags usage);
    // Host-visible buffer whose contents write(mapped) fills directly.
    std::unique_ptr<Buffer> buffer(size_t size, VkBufferUsageFlags usage,
                                   const std::function<void(uint8_t *)> &write);
    // Large sampled images whose last use has completed, reused by image() for
    // an identical description (a per-frame screen overlay of the same size).
    std::vector<std::unique_ptr<Image>> reusableImages;
    void recycle(std::unique_ptr<Image> image);
    std::unique_ptr<Buffer> geometryBuffer(const void *data, size_t size, VkBufferUsageFlags usage,
                                           VkCommandBuffer command);
    std::unique_ptr<Image> image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                                 bool isDepth, VkImageCreateFlags flags = 0,
                                 VkComponentMapping components = {});
    MeshGpu &mesh(const std::shared_ptr<scene::Surface> &source, VkCommandBuffer command);
    TextureGpu &texture(const std::shared_ptr<scene::Texture> &source, VkCommandBuffer command);
    TextureGpu &createTexture(const std::shared_ptr<scene::Texture> &source, VkCommandBuffer command);
    // Uploads the dirty region of an updatable texture into its image.
    void refreshTexture(TextureGpu &value, const scene::Texture &source, VkCommandBuffer command);
    TextureGpu uploadPixels(const std::shared_ptr<scene::Texture> &owner, const scene::Pixels &p,
                            VkCommandBuffer command, VkImageCreateFlags flags);
    TextureGpu &compressedTexture(const std::shared_ptr<scene::Texture> &source, VkCommandBuffer command);
    void copyLevels(VkCommandBuffer command, VkImage image, uint32_t levels,
                    const std::vector<VkBufferImageCopy> &copies);
    bool textureCompression = false; // Device enabled textureCompressionBC.
    float maxAnisotropy = 0;         // Material sampler anisotropy (0: samplerAnisotropy unavailable).
    bool memoryBudget = false;       // VK_EXT_memory_budget queries available.
    // Top mip levels to leave out of a new texture: large textures give up
    // detail as device-local memory fills, instead of failing allocations.
    uint32_t textureMipSkip(uint32_t width, uint32_t height, uint32_t levels) const;
    VkDescriptorSet material(const std::shared_ptr<scene::Material> &source, VkCommandBuffer command);
    // masked: the variant tested against a group's coverage (mask.layout).
    // sprite: the sprite overlay variant (sprite.layout), not for masked draws.
    VkPipeline pipeline(const scene::Draw &draw, bool masked = false, bool sprite = false);
  };
} // namespace remix
