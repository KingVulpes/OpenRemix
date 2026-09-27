#pragma once
#include <atomic>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace remix::pipeline {
class LatestConfiguration {
public:
  uint64_t request(const std::string &key) {
    if (!generation || requested != key) {
      requested = key;
      ++generation;
    }
    return generation;
  }
  bool accepts(uint64_t revision) const { return revision == generation; }

private:
  uint64_t generation = 0;
  std::string requested;
};
// Also used by CPU tests: the compiler is an injected callback. No Vulkan or
// API lock is involved in scheduling, polling or running a job.
class PreparationQueue {
public:
  enum class State { Queued, Running, Complete, Failed, Cancelled };
  struct Job {
    std::atomic<State> state{State::Queued};
    std::atomic<bool> cancelRequested{false};
    std::exception_ptr failure;
    std::function<void()> work;
    bool ready() const { return state.load() >= State::Complete; }
    void check() const;
  };
  using Ticket = std::shared_ptr<Job>;
  PreparationQueue();
  ~PreparationQueue();
  Ticket request(const std::string &key, std::function<void()> work);
  void cancel(const Ticket &job);
  void stop();
  bool onWorker() const;
  // A preparation job can need a pipeline already queued behind it. Claim
  // that job inline on the same worker, rather than waiting on ourselves.
  void finishOnWorker(const Ticket &job);
  void checkCancelled();

private:
  void execute(const Ticket &job);
  void run();
  std::mutex mutex;
  std::condition_variable wake;
  std::deque<Ticket> pending;
  std::vector<Ticket> active;
  std::unordered_map<std::string, Ticket> jobs;
  bool stopping = false;
  std::thread worker;
  static thread_local PreparationQueue *current;
};
} // namespace remix::pipeline
