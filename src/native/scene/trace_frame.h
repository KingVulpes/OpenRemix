#pragma once
#include "scene/types.h"
#include "scene/light_bvh.h"
#include "scene/emission.h"
#include "scene/opacity.h"
#include <algorithm>
#include <vector>

namespace remix::scene {
// CPU snapshots only: safe to construct while the previous GPU submission
// is running. Temporal history and GPU uploads consume this after its fence.
struct TraceFrame {
  struct Surface {
    Draw draw;
    uint32_t animationFrame = 0;
    std::array<float, 4> uv{};
    std::shared_ptr<const OpacityPartition> opacity;
  };
  struct Emitter {
    SphereLight light;
    uint64_t layout = 0;
    bool animated = false;
  };
  // Nonzero caller identities of the frame (open addressing; zero is empty).
  class IdSet {
  public:
    void insert(uint64_t id) {
      if (!id)
        return;
      if ((m_count + 1) * 2 > m_slots.size())
        grow();
      auto &slot = m_slots[locate(id)];
      m_count += slot == 0;
      slot = id;
    }
    size_t count(uint64_t id) const { return id && !m_slots.empty() && m_slots[locate(id)] == id; }
    size_t size() const { return m_count; }

  private:
    size_t locate(uint64_t id) const {
      const size_t mask = m_slots.size() - 1;
      uint64_t h = id * 0x9E3779B97F4A7C15ull;
      for (size_t slot = size_t(h ^ (h >> 29)) & mask;; slot = (slot + 1) & mask)
        if (m_slots[slot] == id || !m_slots[slot])
          return slot;
    }
    void grow() {
      std::vector<uint64_t> old(std::max<size_t>(64, m_slots.size() * 2), 0);
      old.swap(m_slots);
      for (uint64_t id : old)
        if (id)
          m_slots[locate(id)] = id;
    }
    std::vector<uint64_t> m_slots;
    size_t m_count = 0;
  };
  double seconds = 0;
  std::vector<std::vector<Surface>> instances;
  IdSet persistentIds;
  std::vector<Emitter> emitters;
  // Staged copies leave the last submitted lighting intact if presentation is skipped.
  struct Lighting {
    LightBvh combined, torch, emissive;
    EmissionHistory history;
    bool hasEmissive = false;
  } lighting;
};

TraceFrame prepareTraceFrame(const std::vector<Draw> &draws, double seconds, bool filterEmission,
                             SurfaceEmissionCache &emissionCache);
} // namespace remix::scene
