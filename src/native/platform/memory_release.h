#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <vulkan/vulkan.h>

namespace remix {
  // vkFreeMemory off the thread that renders. The driver takes tens of
  // milliseconds to free an allocation (20-35 ms measured for 4 and 32 MiB,
  // on a GPU behind Thunderbolt), and buffers, images and emptied blocks are
  // let go in the middle of a frame. Whatever was bound to the memory is
  // destroyed by the caller first; a mapping ends with the memory.
  class MemoryRelease {
  public:
    static void free(VkDevice device, VkDeviceMemory memory) {
      if (!memory)
        return;
      auto &self = instance();
      std::lock_guard lock(self.m_mutex);
      self.m_queue.emplace_back(device, memory);
      if (!self.m_thread.joinable())
        self.m_thread = std::thread([&self] { self.run(); });
      self.m_wake.notify_one();
    }
    // Everything queued is freed and the thread has ended when this returns:
    // before vkDestroyDevice, and never from a DLL detach path.
    static void drain() {
      auto &self = instance();
      std::thread thread;
      {
        std::lock_guard lock(self.m_mutex);
        self.m_stop = true;
        thread = std::move(self.m_thread);
        self.m_wake.notify_one();
      }
      if (thread.joinable())
        thread.join();
      std::lock_guard lock(self.m_mutex);
      self.m_stop = false;
    }

  private:
    static MemoryRelease &instance() {
      static MemoryRelease *self = new MemoryRelease; // never destroyed: no thread joins at exit
      return *self;
    }
    void run() {
      std::unique_lock lock(m_mutex);
      for (;;) {
        m_wake.wait(lock, [&] { return m_stop || !m_queue.empty(); });
        if (m_queue.empty())
          return;
        const auto [device, memory] = m_queue.front();
        m_queue.pop_front();
        lock.unlock();
        vkFreeMemory(device, memory, nullptr);
        lock.lock();
      }
    }
    std::mutex m_mutex;
    std::condition_variable m_wake;
    std::deque<std::pair<VkDevice, VkDeviceMemory>> m_queue;
    std::thread m_thread;
    bool m_stop = false;
  };
} // namespace remix
