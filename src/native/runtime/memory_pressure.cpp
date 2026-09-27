#include "runtime/memory_pressure.h"
#include <algorithm>

namespace remix {
  void MemoryPressure::update(uint64_t usage, uint64_t budget, uint32_t frame) {
    constexpr double thresholds[4] = {0, elevated, high, critical};
    uint64_t release = 0;
    if (!budget) {
      m_level = 0;
      m_relax = 0;
      usage = 0;
    } else {
      const double fraction = double(usage) / double(budget);
      uint32_t target = 0;
      for (uint32_t level = 1; level <= 3; ++level)
        if (fraction >= thresholds[level])
          target = level;
      if (target > m_level) {
        // One step at a time, so the host's response can land before the next.
        m_relax = 0;
        if (m_level == 0 || frame - m_lastChange >= escalateFrames) {
          ++m_level;
          m_lastChange = frame;
        }
      } else if (m_level > 0 && fraction < thresholds[m_level] - hysteresis) {
        if (++m_relax >= relaxFrames) {
          --m_level;
          m_lastChange = frame;
          m_relax = 0;
        }
      } else {
        m_relax = 0;
      }
      // Usage above the point at which level 1 would be left.
      const double point = std::max(elevated - hysteresis, 0.0) * double(budget);
      release = double(usage) > point ? uint64_t(double(usage) - point) : 0;
    }
    m_publishedLevel.store(m_level, std::memory_order_relaxed);
    m_request.store(m_level >= hostSignalLevel ? 1u : 0u, std::memory_order_relaxed);
    m_usage.store(usage, std::memory_order_relaxed);
    m_budget.store(budget, std::memory_order_relaxed);
    m_release.store(release, std::memory_order_relaxed);
    m_frame.store(frame, std::memory_order_relaxed);
  }

  MemoryPressureSample MemoryPressure::sample() const {
    MemoryPressureSample s;
    s.level = m_publishedLevel.load(std::memory_order_relaxed);
    s.requestHostRelease = m_request.load(std::memory_order_relaxed);
    s.frame = m_frame.load(std::memory_order_relaxed);
    s.usageBytes = m_usage.load(std::memory_order_relaxed);
    s.budgetBytes = m_budget.load(std::memory_order_relaxed);
    s.bytesToRelease = m_release.load(std::memory_order_relaxed);
    return s;
  }
} // namespace remix
