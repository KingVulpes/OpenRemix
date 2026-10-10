#include "scene/micromap.h"

namespace remix::scene {
  namespace {
    // XOR of each bit with every higher bit of its 16-bit half.
    uint32_t prefixXor(uint32_t x) {
      x ^= (x >> 1) & 0x7fff7fffu;
      x ^= (x >> 2) & 0x3fff3fffu;
      x ^= (x >> 4) & 0x0fff0fffu;
      x ^= (x >> 8) & 0x00ff00ffu;
      return x;
    }
    uint32_t spread(uint32_t x) {
      x &= 0xffffu;
      x = (x | (x << 8)) & 0x00ff00ffu;
      x = (x | (x << 4)) & 0x0f0f0f0fu;
      x = (x | (x << 2)) & 0x33333333u;
      return (x | (x << 1)) & 0x55555555u;
    }
  } // namespace

  uint32_t microTriangleIndex(uint32_t u, uint32_t v, uint32_t w, uint32_t level) {
    const uint32_t mask = (1u << level) - 1;
    const uint32_t b0 = ~(u ^ w) & mask;
    const uint32_t t = (u ^ v) & b0;
    const uint32_t c = (((u & v & w) | (~u & ~v & ~w)) & mask) << 16;
    const uint32_t f = prefixXor(t | c) ^ u;
    const uint32_t b1 = (f & ~b0) | t;
    return spread(b0) | spread(b1) << 1;
  }

  const std::vector<MicroTriangle> &microTriangles(uint32_t level) {
    static const auto tables = [] {
      std::array<std::vector<MicroTriangle>, OpacityMicromap::maxLevel + 1> result;
      for (uint32_t l = 0; l <= OpacityMicromap::maxLevel; ++l) {
        const uint32_t n = 1u << l;
        auto &table = result[l];
        table.resize(size_t(n) * n);
        for (uint32_t j = 0; j < n; ++j)
          for (uint32_t i = 0; i + j < n; ++i) {
            table[microTriangleIndex(i, j, n - 1 - i - j, l)] = {uint16_t(i), uint16_t(j), true};
            if (i + j + 2 <= n)
              table[microTriangleIndex(i, j, n - 2 - i - j, l)] = {uint16_t(i), uint16_t(j), false};
          }
      }
      return result;
    }();
    return tables[level];
  }

  std::array<std::array<float, 2>, 3> microCorners(MicroTriangle m, uint32_t level) {
    const float step = 1.f / float(1u << level), i = m.i * step, j = m.j * step;
    if (m.upright)
      return {{{i, j}, {i + step, j}, {i, j + step}}};
    return {{{i + step, j}, {i, j + step}, {i + step, j + step}}};
  }
} // namespace remix::scene
