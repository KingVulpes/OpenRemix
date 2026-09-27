#include "scene/types.h"
#include <cstring>

namespace remix::scene {
  uint64_t hashBytes(const void *bytes, size_t size, uint64_t hash) {
    const auto *p = static_cast<const uint8_t *>(bytes);
    for (size_t i = 0; i < size; ++i)
      hash = (hash ^ p[i]) * 1099511628211ull;
    return hash;
  }

  // xxHash64 (public-domain algorithm by Yann Collet).
  uint64_t fastHash(const void *bytes, size_t size, uint64_t seed) {
    constexpr uint64_t p1 = 0x9E3779B185EBCA87ull, p2 = 0xC2B2AE3D27D4EB4Full, p3 = 0x165667B19E3779F9ull,
                       p4 = 0x85EBCA77C2B2AE63ull, p5 = 0x27D4EB2F165667C5ull;
    auto rotate = [](uint64_t v, int r) { return (v << r) | (v >> (64 - r)); };
    auto read64 = [](const uint8_t *p) {
      uint64_t v;
      std::memcpy(&v, p, sizeof(v));
      return v;
    };
    auto round = [&](uint64_t acc, uint64_t input) { return rotate(acc + input * p2, 31) * p1; };
    auto merge = [&](uint64_t acc, uint64_t lane) { return (acc ^ round(0, lane)) * p1 + p4; };
    const auto *p = static_cast<const uint8_t *>(bytes);
    const auto *end = p + size;
    uint64_t hash;
    if (size >= 32) {
      uint64_t v1 = seed + p1 + p2, v2 = seed + p2, v3 = seed, v4 = seed - p1;
      for (const auto *limit = end - 32; p <= limit; p += 32) {
        v1 = round(v1, read64(p));
        v2 = round(v2, read64(p + 8));
        v3 = round(v3, read64(p + 16));
        v4 = round(v4, read64(p + 24));
      }
      hash = rotate(v1, 1) + rotate(v2, 7) + rotate(v3, 12) + rotate(v4, 18);
      hash = merge(merge(merge(merge(hash, v1), v2), v3), v4);
    } else
      hash = seed + p5;
    hash += uint64_t(size);
    for (; p + 8 <= end; p += 8)
      hash = rotate(hash ^ round(0, read64(p)), 27) * p1 + p4;
    if (p + 4 <= end) {
      uint32_t v;
      std::memcpy(&v, p, sizeof(v));
      hash = rotate(hash ^ (uint64_t(v) * p1), 23) * p2 + p3;
      p += 4;
    }
    for (; p < end; ++p)
      hash = rotate(hash ^ (uint64_t(*p) * p5), 11) * p1;
    hash ^= hash >> 33;
    hash *= p2;
    hash ^= hash >> 29;
    hash *= p3;
    hash ^= hash >> 32;
    return hash;
  }
} // namespace remix::scene
