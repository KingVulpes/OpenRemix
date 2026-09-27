#pragma once
#include <atomic>
#include <cstdint>

namespace remix {
  // Device-local memory pressure for remixapi_GetMemoryPressure, with the
  // levels, hysteresis and release point of the dxvk-remix fork's
  // rtx.vramBudget controller at its default settings. OpenRemix has no
  // runtime-side levers to pull, so the levels only inform the host; the
  // host is asked to release far content at the critical level.
  struct MemoryPressureSample {
    uint32_t level = 0, requestHostRelease = 0, frame = 0;
    uint64_t usageBytes = 0, budgetBytes = 0, bytesToRelease = 0;
  };

  class MemoryPressure {
  public:
    // Fractions of the driver budget at which levels 1-3 start.
    static constexpr double elevated = .90, high = .94, critical = .97;
    // A level is left once usage stays this far below its threshold for
    // relaxFrames consecutive updates; levels rise at most one per
    // escalateFrames updates.
    static constexpr double hysteresis = .04;
    static constexpr uint32_t relaxFrames = 600, escalateFrames = 15;
    static constexpr uint32_t hostSignalLevel = 3;

    // Once per frame (API mutex held). budget 0 means off or unavailable:
    // everything but the frame reads zero.
    void update(uint64_t usage, uint64_t budget, uint32_t frame);
    // Any thread, no locks. Values are at most one update old.
    MemoryPressureSample sample() const;

  private:
    uint32_t m_level = 0, m_lastChange = 0, m_relax = 0;
    std::atomic<uint32_t> m_publishedLevel{0}, m_request{0}, m_frame{0};
    std::atomic<uint64_t> m_usage{0}, m_budget{0}, m_release{0};
  };
} // namespace remix
