#pragma once
#include "scene/types.h"
#include "scene/light_bvh.h"
#include "scene/emission.h"
#include <unordered_set>

namespace remix::scene {
// CPU snapshots only: safe to construct while the previous GPU submission
// is running. Temporal history and GPU uploads consume this after its fence.
struct TraceFrame {
  struct Surface {
    Draw draw;
    uint32_t animationFrame = 0;
    std::array<float, 4> uv{};
  };
  struct Emitter {
    SphereLight light;
    uint64_t layout = 0;
    bool animated = false;
  };
  double seconds = 0;
  std::vector<std::vector<Surface>> instances;
  std::unordered_set<uint64_t> persistentIds;
  std::vector<std::array<float, 4>> emissions{1}; // zero is non-emissive
  std::vector<Emitter> emitters;
  // Staged copies leave the last submitted lighting intact if presentation is skipped.
  struct Lighting {
    LightBvh combined, torch, emissive;
    EmissionHistory history;
    bool hasEmissive = false;
  } lighting;
};

TraceFrame prepareTraceFrame(const std::vector<Draw> &draws, double seconds, bool filterEmission);
} // namespace remix::scene
