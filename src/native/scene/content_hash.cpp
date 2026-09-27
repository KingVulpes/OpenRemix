#include "scene/types.h"
#include <functional>
#include <string_view>

namespace remix::scene {
  uint64_t hashBytes(const void *bytes, size_t size, uint64_t hash) {
    const auto *p = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0; i < size; ++i)
      hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
  }
  void Signature::append(const void *data, size_t size) {
    // Bulk-hash each range, then fold the digest and its extent into two chains.
    const uint64_t digest = std::hash<std::string_view>{}({static_cast<const char *>(data), size});
    first = (first ^ digest) * 0x9e3779b185ebca87ull + size;
    first = (first << 27 | first >> 37) ^ (first >> 31);
    second = (second ^ digest ^ (bytes << 32)) * 0xff51afd7ed558ccdull;
    second ^= second >> 33;
    bytes += size;
  }
} // namespace remix::scene
