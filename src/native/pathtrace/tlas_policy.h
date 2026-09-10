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
    bool operator==(const InstanceInput &) const = default;
  };
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
