#include "runtime/memory_pressure.h"
#include <cstdio>
#include <cstdlib>

// remixapi_GetMemoryPressure levels: the fork's rtx.vramBudget defaults
// (90/94/97%, one step per 15 frames, 4% hysteresis held for 600 frames,
// release point at 86%, host asked at the critical level).
namespace {
  void require(bool value, const char *what) {
    if (!value) {
      std::fprintf(stderr, "Memory pressure test failed: %s\n", what);
      std::exit(1);
    }
  }
} // namespace

int main() {
  using remix::MemoryPressure;
  MemoryPressure p;
  constexpr uint64_t budget = 1000ull << 20;
  auto at = [&](double fraction) { return uint64_t(fraction * double(budget)); };
  uint32_t frame = 0;
  auto run = [&](double fraction, uint32_t frames) {
    for (uint32_t i = 0; i < frames; ++i)
      p.update(at(fraction), budget, ++frame);
    return p.sample();
  };

  auto s = run(.5, 1);
  require(s.level == 0 && !s.requestHostRelease && s.bytesToRelease == 0 && s.usageBytes == at(.5) &&
              s.budgetBytes == budget && s.frame == frame,
          "Low usage reported pressure");

  // Critical usage climbs one level at a time: at once from none, then every 15 frames.
  s = run(.99, 1);
  require(s.level == 1 && !s.requestHostRelease, "First step was not immediate");
  s = run(.99, 14);
  require(s.level == 1, "Escalated before escalateFrames");
  s = run(.99, 1);
  require(s.level == 2, "Did not escalate to high");
  s = run(.99, 15);
  require(s.level == 3 && s.requestHostRelease == 1, "Critical level did not ask the host");
  const uint64_t point = uint64_t((MemoryPressure::elevated - MemoryPressure::hysteresis) * double(budget));
  require(s.bytesToRelease == at(.99) - point, "bytesToRelease is not usage above the level-1 release point");

  // Inside the hysteresis band the level holds indefinitely.
  s = run(.95, 2000);
  require(s.level == 3, "Level dropped inside the hysteresis band");
  // Below it, one level per 600 consecutive frames.
  s = run(.90, 599);
  require(s.level == 3, "Level dropped early");
  s = run(.90, 1);
  require(s.level == 2 && !s.requestHostRelease, "Level did not relax after relaxFrames");
  s = run(.85, 1200);
  require(s.level == 0 && s.bytesToRelease == 0, "Pressure did not relax fully");

  // Off / no VK_EXT_memory_budget: everything but the frame is zero.
  run(.99, 40);
  p.update(at(.99), 0, ++frame);
  s = p.sample();
  require(s.level == 0 && !s.requestHostRelease && !s.usageBytes && !s.budgetBytes && !s.bytesToRelease &&
              s.frame == frame,
          "Disabled pressure reported values");
  std::puts("Memory pressure levels, hysteresis, host signal and release amount passed");
  return 0;
}
