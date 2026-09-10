#pragma once
#include "raster/renderer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
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
    ~Image() {
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
    bool encodeOutput = false;
    VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkShaderModule vertex = VK_NULL_HANDLE, fragment = VK_NULL_HANDLE;
    std::unique_ptr<Image> depth;
    struct MeshGpu {
      std::weak_ptr<scene::Surface> owner;
      std::unique_ptr<Buffer> data;
      VkDeviceSize indexOffset;
    };
    struct TextureGpu {
      std::weak_ptr<scene::Texture> owner;
      std::unique_ptr<Image> data;
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
          vkDestroySampler(device, sampler, nullptr);
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
    std::vector<Command> commands;
    std::chrono::steady_clock::time_point epoch = std::chrono::steady_clock::now();

    ~Gpu();
    void destroyTarget();
    uint32_t memoryType(uint32_t bits, VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> allocateBuffer(size_t size, VkBufferUsageFlags usage,
                                           VkMemoryPropertyFlags flags);
    std::unique_ptr<Buffer> buffer(const void *data, size_t size, VkBufferUsageFlags usage);
    std::unique_ptr<Buffer> geometryBuffer(const void *data, size_t size, VkBufferUsageFlags usage,
                                           VkCommandBuffer command);
    std::unique_ptr<Image> image(uint32_t width, uint32_t height, uint32_t levels, VkFormat format,
                                 bool isDepth);
    MeshGpu &mesh(const std::shared_ptr<scene::Surface> &source, VkCommandBuffer command);
    TextureGpu &texture(const std::shared_ptr<scene::Texture> &source, VkCommandBuffer command);
    VkDescriptorSet material(const std::shared_ptr<scene::Material> &source, VkCommandBuffer command);
    VkPipeline pipeline(const scene::Draw &draw);
  };
} // namespace remix
