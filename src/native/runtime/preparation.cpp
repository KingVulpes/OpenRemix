#include "runtime/preparation.h"
#include <stdexcept>

namespace remix::pipeline {
thread_local PreparationQueue *PreparationQueue::current = nullptr;
void PreparationQueue::Job::check() const {
  const auto value = state.load();
  if (value == State::Failed)
    std::rethrow_exception(failure);
  if (value != State::Complete)
    throw std::runtime_error("Preparation cancelled or incomplete");
}
PreparationQueue::PreparationQueue() : worker([this] { run(); }) {}
PreparationQueue::~PreparationQueue() { stop(); }
bool PreparationQueue::onWorker() const { return current == this; }
PreparationQueue::Ticket PreparationQueue::request(const std::string &key,
                                                   std::function<void()> work) {
  std::lock_guard lock(mutex);
  if (auto it = jobs.find(key); it != jobs.end())
    return it->second;
  auto job = std::make_shared<Job>();
  if (stopping)
    job->state = State::Cancelled;
  else {
    job->work = std::move(work);
    pending.push_back(job);
  }
  jobs.emplace(key, job);
  wake.notify_one();
  return job;
}
void PreparationQueue::cancel(const Ticket &job) {
  if (!job)
    return;
  std::function<void()> discarded;
  {
    std::lock_guard lock(mutex);
    job->cancelRequested = true;
    if (job->state == State::Queued) {
      discarded = std::move(job->work);
      job->state = State::Cancelled;
    }
  }
}
void PreparationQueue::stop() {
  std::vector<std::function<void()>> discarded;
  {
    std::lock_guard lock(mutex);
    stopping = true;
    for (auto &job : active)
      job->cancelRequested = true;
    for (auto &job : pending) {
      if (job->state != State::Queued)
        continue;
      discarded.push_back(std::move(job->work));
      job->state = State::Cancelled;
    }
    pending.clear();
  }
  wake.notify_one();
  if (worker.joinable())
    worker.join();
  // Captures may own SDK objects. Release them only after the executing SDK
  // call has returned, and without the queue mutex held.
  discarded.clear();
}
void PreparationQueue::finishOnWorker(const Ticket &job) {
  if (!onWorker())
    throw std::logic_error("Only the compiler worker can execute jobs");
  checkCancelled();
  execute(job);
  job->check();
}
void PreparationQueue::checkCancelled() {
  if (!onWorker())
    return;
  std::lock_guard lock(mutex);
  for (const auto &job : active)
    if (job->cancelRequested)
      throw std::runtime_error("Preparation superseded or stopped");
}
void PreparationQueue::execute(const Ticket &job) {
  std::function<void()> work;
  {
    std::lock_guard lock(mutex);
    if (job->state != State::Queued)
      return;
    job->state = State::Running;
    active.push_back(job);
    work = std::move(job->work);
  }
  try {
    work();
    // Release captures before publishing completion (including device objects).
    work = {};
    job->state = State::Complete;
  } catch (...) {
    job->failure = std::current_exception();
    work = {};
    job->state = State::Failed;
  }
  {
    std::lock_guard lock(mutex);
    active.pop_back();
  }
}
void PreparationQueue::run() {
  current = this;
  for (;;) {
    Ticket job;
    {
      std::unique_lock lock(mutex);
      wake.wait(lock, [&] { return stopping || !pending.empty(); });
      if (stopping)
        break;
      job = std::move(pending.front());
      pending.pop_front();
    }
    execute(job);
  }
  current = nullptr;
}
} // namespace remix::pipeline
