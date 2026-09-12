#pragma once
#include "scene/types.h"

namespace remix::scene {
// Cached, deterministic area quadrature in UV space. No emission texture reads
// occur in lighting rays. Texture snapshots own the cache's lifetime.
std::array<float, 3> triangleEmission(const Draw &draw, uint32_t triangle, const float *uvTransform);
SphereLight triangleEmitter(const Draw &draw, uint32_t triangle, const std::array<float, 3> &radiance);
// Excludes texture contents and sprite frame: only animation may be smoothed.
uint64_t emissionHistoryKey(const Draw &draw, uint32_t triangle);
class EmissionHistory {
public:
  void begin(uint64_t world, double seconds, uint32_t mode); // 0 raw, 1 smooth, 2 diagnostic freeze
  void apply(SphereLight &light, uint64_t layout);
  void end();
  size_t size() const { return entries.size(); }
  double rawChange = 0, filteredChange = 0;
private:
  struct Entry {
    uint64_t layout = 0, seen = 0;
    std::array<float, 3> raw{}, filtered{};
  };
  std::unordered_map<uint64_t, Entry> entries;
  uint64_t world = 0, sequence = 0;
  uint32_t mode = 0;
  double time = 0, alpha = 1;
};
}
