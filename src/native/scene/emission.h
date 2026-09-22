#pragma once
#include "scene/types.h"
#include <map>
#include <tuple>

namespace remix::scene {
// Cached, deterministic area quadrature in UV space. No emission texture reads
// occur in lighting rays. Texture snapshots own the cache's lifetime.
std::array<float, 3> triangleEmission(const Draw &draw, uint32_t triangle, const float *uvTransform);
SphereLight triangleEmitter(const Draw &draw, uint32_t triangle, const std::array<float, 3> &radiance);
// Excludes texture contents and sprite frame: only animation may be smoothed.
uint64_t emissionHistoryKey(const Draw &draw, uint32_t triangle);
// Reduce immutable geometry once per emission/UV snapshot. Keep only luminous
// triangles: a shared terrain atlas usually leaves most of the mesh black.
// World transforms, skinning, identities and sidedness are applied per frame.
class SurfaceEmissionCache {
public:
  struct Triangle {
    uint32_t index = 0;
    std::array<float, 3> radiance{};
    uint64_t layout = 0;
  };
  using Reduction = std::vector<Triangle>;
  std::shared_ptr<const Reduction> reduce(const Draw &draw, const std::array<float, 4> &uv);
  void prune();

private:
  using Key = std::tuple<std::array<float, 3>, std::array<float, 4>,
                         uint8_t, uint8_t, uint8_t, uint8_t, uint8_t>;
  struct Entry {
    std::weak_ptr<Texture> texture;
    bool textured = false;
    std::shared_ptr<const Reduction> reduction;
  };
  struct SurfaceEntries {
    std::weak_ptr<Surface> owner;
    std::map<Key, Entry> entries;
  };
  std::unordered_map<const Surface *, SurfaceEntries> surfaces;
};
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
