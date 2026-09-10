#include "scene/types.h"

namespace remix::scene {
  uint64_t hashBytes(const void *bytes, size_t size, uint64_t hash) {
    const auto *p = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0; i < size; ++i)
      hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
  }
} // namespace remix::scene
