#pragma once
#include <vulkan/vulkan.h>
#include <array>
#include <memory>
#include <optional>

namespace remix::trace::detail {
  struct BufferAllocation {
    VkDevice device = VK_NULL_HANDLE;
    VkBuffer handle = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceAddress address = 0;
    VkDeviceSize size = 0, allocationSize = 0;
    VkBufferUsageFlags usage = 0;
    bool host = false;

    void destroy() const noexcept {
      if (handle)
        vkDestroyBuffer(device, handle, nullptr);
      if (memory)
        vkFreeMemory(device, memory, nullptr);
    }
  };

  // Only upload storage participates. Buffer owners retain the usual submission-slot
  // references; an allocation becomes reusable when its final owner releases it.
  // Fixed storage keeps that release path allocation-free and caps idle memory.
  class BufferCache {
  public:
    static constexpr VkDeviceSize byteLimit = 64 * 1024 * 1024;
    static constexpr size_t entryLimit = 512;

    ~BufferCache() {
      for (size_t i = 0; i < m_count; ++i)
        m_entries[i].destroy();
    }
    BufferCache() = default;
    BufferCache(const BufferCache &) = delete;
    BufferCache &operator=(const BufferCache &) = delete;

    std::optional<BufferAllocation> take(VkDeviceSize size, VkBufferUsageFlags usage, bool host) {
      size_t best = m_count;
      for (size_t i = 0; i < m_count; ++i) {
        const auto &entry = m_entries[i];
        if (entry.usage == usage && entry.host == host && entry.size >= size &&
            entry.size - size <= size && (best == m_count || entry.size < m_entries[best].size))
          best = i;
      }
      if (best == m_count)
        return {};
      const auto entry = m_entries[best];
      m_bytes -= entry.allocationSize;
      m_entries[best] = m_entries[--m_count];
      return entry;
    }

    bool put(const BufferAllocation &entry) noexcept {
      if (m_count == entryLimit || entry.allocationSize > byteLimit - m_bytes)
        return false;
      m_entries[m_count++] = entry;
      m_bytes += entry.allocationSize;
      return true;
    }

  private:
    std::array<BufferAllocation, entryLimit> m_entries{};
    size_t m_count = 0;
    VkDeviceSize m_bytes = 0;
  };

  struct Buffer : BufferAllocation {
    std::shared_ptr<BufferCache> cache;
    Buffer() = default;
    Buffer(const Buffer &) = delete;
    Buffer &operator=(const Buffer &) = delete;
    ~Buffer() {
      if (!cache || !cache->put(*this))
        destroy();
    }
  };
} // namespace remix::trace::detail
