#pragma once
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

namespace remix {
  // One dedicated thread that runs the jobs handed to it in order (the
  // presents of frame generation: a frame's images are shown over the course
  // of the frame, while the host already prepares the next one or two).
  // submit() queues and returns; wait(n) blocks until at most n jobs are
  // unfinished. The owner serializes submit(), wait() and stop(). Jobs must
  // not throw (one that does is dropped with its exception).
  class PresentQueue {
  public:
    PresentQueue() = default;
    ~PresentQueue() { stop(); }
    PresentQueue(const PresentQueue &) = delete;
    PresentQueue &operator=(const PresentQueue &) = delete;

    bool started() const { return m_thread.joinable(); }

    void submit(std::function<void()> job) {
      if (!m_thread.joinable())
        m_thread = std::thread([this] { run(); });
      {
        std::lock_guard lock(m_mutex);
        m_jobs.push_back(std::move(job));
        ++m_unfinished;
      }
      m_wake.notify_one();
    }

    void wait(size_t atMost = 0) {
      std::unique_lock lock(m_mutex);
      m_done.wait(lock, [&] { return m_unfinished <= atMost; });
    }

    // Finishes the queued jobs and joins the thread. The next submit() starts
    // a new one.
    void stop() {
      if (!m_thread.joinable())
        return;
      wait();
      {
        std::lock_guard lock(m_mutex);
        m_stop = true;
      }
      m_wake.notify_one();
      m_thread.join();
      m_stop = false;
    }

  private:
    void run() {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock(m_mutex);
          m_wake.wait(lock, [this] { return m_stop || !m_jobs.empty(); });
          if (m_jobs.empty())
            return;
          job = std::move(m_jobs.front());
          m_jobs.pop_front();
        }
        try {
          job();
        } catch (...) {
        }
        {
          std::lock_guard lock(m_mutex);
          --m_unfinished;
        }
        m_done.notify_all();
      }
    }

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_wake, m_done;
    std::deque<std::function<void()>> m_jobs;
    size_t m_unfinished = 0;
    bool m_stop = false;
  };
} // namespace remix
