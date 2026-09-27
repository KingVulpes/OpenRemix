#pragma once
#include "scene/types.h"
#include <remix/remix_native_ext.h>

namespace remix::trace {
  struct InstanceInput {
    uint64_t id = 0, geometry = 0;
    uint32_t sub = 0, customIndex = 0;
    scene::Matrix model = scene::identity();
    float diagonal = 0;
    uint64_t boundsRevision = 0;
    // Every surface is one-sided: hardware culls back faces for this instance.
    bool facingCulled = false;
    uint8_t mask = 0xff;
    bool operator==(const InstanceInput &) const = default;
  };
  // Facing culling is per TLAS instance; one double-sided surface disables it.
  template <class Surfaces> bool facingCulled(const Surfaces &instance) {
    for (const auto &surface : instance)
      if (surface.draw.doubleSided)
        return false;
    return !instance.empty();
  }
  // Shadow rays exclude decal-only instances in hardware (see visibility.glsl);
  // mixed instances keep the per-surface decal test.
  inline constexpr uint8_t traceMask = 1, decalMask = 2;
  template <class Surfaces> uint8_t instanceMask(const Surfaces &instance) {
    for (const auto &surface : instance)
      if (!surface.draw.decal)
        return traceMask;
    return instance.empty() ? traceMask : decalMask;
  }
  struct TlasPolicy {
    std::vector<InstanceInput> previous, built;
    uint32_t refits = 0;
    bool initialized = false;
    // NONE + unchanged means no GPU work; NONE + changed means an UPDATE.
    uint32_t reason(const std::vector<InstanceInput> &inputs, bool dynamic, bool force) const;
    bool changed(const std::vector<InstanceInput> &inputs) const;
    void commit(const std::vector<InstanceInput> &inputs, uint32_t reason);
  };
} // namespace remix::trace
