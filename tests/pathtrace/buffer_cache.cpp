#include "pathtrace/buffer_cache.h"
#include <cassert>
#include <cstdio>
#include <unordered_set>

using namespace remix::trace::detail;

namespace {
  std::unordered_set<VkBuffer> destroyed;
  std::unordered_set<VkDeviceMemory> freed;
  uintptr_t nextHandle = 1;

  std::shared_ptr<Buffer> allocation(const std::shared_ptr<BufferCache> &cache, VkDeviceSize size,
                                     VkBufferUsageFlags usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                                     bool host = true) {
    auto result = std::make_shared<Buffer>();
    result->handle = reinterpret_cast<VkBuffer>(nextHandle);
    result->memory = reinterpret_cast<VkDeviceMemory>(nextHandle++);
    result->size = result->allocationSize = size;
    result->usage = usage;
    result->host = host;
    result->cache = cache;
    return result;
  }

  std::shared_ptr<Buffer> restore(std::shared_ptr<BufferCache> cache, BufferAllocation entry) {
    auto result = std::make_shared<Buffer>();
    static_cast<BufferAllocation &>(*result) = entry;
    result->cache = std::move(cache);
    return result;
  }
}

// Mock only destruction: these tests exercise ownership and matching without a Vulkan device.
extern "C" VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer buffer,
                                                       const VkAllocationCallbacks *) {
  assert(destroyed.insert(buffer).second);
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory memory,
                                                   const VkAllocationCallbacks *) {
  assert(freed.insert(memory).second);
}

int main() {
  {
    auto cache = std::make_shared<BufferCache>();
    auto buffer = allocation(cache, 4096);
    auto pendingFrame = buffer;
    const auto handle = buffer->handle;
    buffer.reset();
    assert(!cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
    pendingFrame.reset();
    assert(!destroyed.count(handle));
    auto reused = cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    assert(reused && reused->handle == handle);
    assert(!cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
    auto owner = restore(cache, *reused);
    cache.reset(); // The last outstanding buffer also keeps the cache alive.
    assert(!destroyed.count(handle));
    owner.reset();
    assert(destroyed.count(handle));
  }
  {
    auto cache = std::make_shared<BufferCache>();
    allocation(cache, 16384).reset();
    auto smaller = allocation(cache, 8192);
    const auto handle = smaller->handle;
    smaller.reset();
    assert(!cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false));
    assert(!cache->take(4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true));
    assert(!cache->take(1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
    auto best = cache->take(4096, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    assert(best && best->handle == handle);
    restore(cache, *best).reset();
    assert(!cache->take(32768, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true));
  }
  {
    auto cache = std::make_shared<BufferCache>();
    allocation(cache, BufferCache::byteLimit).reset();
    auto overflow = allocation(cache, 4096);
    const auto handle = overflow->handle;
    overflow.reset();
    assert(destroyed.count(handle));
    auto large = cache->take(BufferCache::byteLimit, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true);
    assert(large);
    large->destroy();
    auto smaller = allocation(cache, 4096);
    const auto smallerHandle = smaller->handle;
    smaller.reset();
    assert(!destroyed.count(smallerHandle)); // Taking an entry restores the byte allowance.
  }
  {
    auto cache = std::make_shared<BufferCache>();
    for (size_t i = 0; i < BufferCache::entryLimit; ++i)
      allocation(cache, 4096).reset();
    auto overflow = allocation(cache, 4096);
    const auto handle = overflow->handle;
    overflow.reset();
    assert(destroyed.count(handle));
  }
  assert(destroyed.size() == nextHandle - 1 && freed.size() == destroyed.size());
  std::puts("Upload buffer cache ownership, compatibility, limits and cleanup passed");
}
