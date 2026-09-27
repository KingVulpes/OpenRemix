// RenderThread (asynchronous presentation): jobs run on one dedicated thread,
// at most one at a time; submit() waits only for the job in flight and reports
// that wait; errors are kept; stop() drains and joins; the submitting thread's
// floating-point modes carry over. CPU only.
#include "platform/render_thread.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <thread>

using namespace remix;
using namespace std::chrono_literals;

namespace {
  void require(bool value, const char *what) {
    if (!value) {
      std::fprintf(stderr, "Render thread test failed: %s\n", what);
      std::exit(1);
    }
  }

  void runsElsewhere() {
    RenderThread thread;
    require(!thread.started() && thread.idle() && thread.wait() == 0, "A new render thread is not idle");
    std::thread::id worker;
    const double waited = thread.submit([&] { worker = std::this_thread::get_id(); });
    require(waited == 0, "The first submit waited");
    require(thread.started(), "The render thread did not start");
    thread.wait();
    require(worker != std::thread::id{} && worker != std::this_thread::get_id(), "The job ran on the caller");
    require(thread.idle() && thread.submitted() == 1 && thread.completed() == 1, "Job counts");
  }

  void oneInFlight() {
    RenderThread thread;
    std::atomic<int> running{0}, most{0}, done{0};
    auto job = [&] {
      const int now = ++running;
      int seen = most.load();
      while (now > seen && !most.compare_exchange_weak(seen, now)) {
      }
      std::this_thread::sleep_for(30ms);
      --running;
      ++done;
    };
    thread.submit(job);
    // The caller continues at once: the job is still running.
    require(!thread.idle() && done == 0, "submit waited for its own job");
    const double waited = thread.submit(job); // Waits for the first job only.
    require(waited > 0 && done == 1, "The second submit did not wait for the frame in flight");
    thread.submit(job);
    thread.wait();
    require(done == 3 && most == 1, "Jobs overlapped");
    require(thread.completed() == 3, "A job was lost");
  }

  void keepsErrors() {
    RenderThread thread;
    thread.submit([] { throw std::runtime_error("render failure"); });
    thread.wait();
    require(bool(thread.takeError()) && !thread.takeError(), "The job's exception was not kept once");
    bool ran = false;
    thread.submit([&] { ran = true; });
    thread.wait();
    require(ran, "The render thread stopped after an exception");
  }

  void stopsAndRestarts() {
    RenderThread thread;
    std::atomic<bool> finished{false};
    thread.submit([&] {
      std::this_thread::sleep_for(20ms);
      finished = true;
    });
    thread.stop(); // Drains the job in flight, then joins.
    require(finished && !thread.started(), "stop() did not drain and join");
    bool ran = false;
    thread.submit([&] { ran = true; });
    thread.wait();
    require(ran && thread.started(), "The render thread did not restart");
    // Destruction with a job in flight drains it first.
    auto *owned = new RenderThread;
    std::atomic<bool> late{false};
    owned->submit([&] {
      std::this_thread::sleep_for(20ms);
      late = true;
    });
    delete owned;
    require(late, "Destruction abandoned the job in flight");
  }

  void floatingPointModes() {
#if defined(REMIX_RENDER_THREAD_MXCSR)
    const unsigned saved = _mm_getcsr();
    // Flush-to-zero and denormals-are-zero, as a host game may set them.
    _mm_setcsr(saved | 0x8040u);
    RenderThread thread;
    unsigned seen = 0;
    thread.submit([&] { seen = _mm_getcsr(); });
    thread.wait();
    _mm_setcsr(saved);
    require((seen & 0x8040u) == 0x8040u, "The job did not run with the caller's floating-point modes");
    unsigned restored = 0;
    thread.submit([&] { restored = _mm_getcsr(); });
    thread.wait();
    require((restored & 0x8040u) == (saved & 0x8040u), "The job kept a previous caller's floating-point modes");
#endif
  }
} // namespace

int main() {
  runsElsewhere();
  oneInFlight();
  keepsErrors();
  stopsAndRestarts();
  floatingPointModes();
  std::puts("render thread handoff, draining and errors");
}
