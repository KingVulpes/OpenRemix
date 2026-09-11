#pragma once
#include "scene/types.h"
#include <vector>

namespace remix::scene {
// std430 records shared with light_bvh.glsl. Analytic emitters need no
// geometry.
struct LightEmitter {
  float positionRadius[4] = {};
  float radiance[4] = {};
};
struct LightNode {
  float minimumPower[4] = {};
  float maximum[4] = {};  // xyz bounds; w largest emitter radius squared
  uint32_t links[4] = {}; // left, right, emitter, leaf count
};
static_assert(sizeof(LightEmitter) == 32 && sizeof(LightNode) == 48);
class LightBvh {
public:
  bool update(std::vector<SphereLight> lights);
  const std::vector<LightEmitter> &emitters() const { return m_emitters; }
  const std::vector<LightNode> &nodes() const { return m_nodes; }
  uint64_t generation = 0, builds = 0, refits = 0;
  uint32_t active = 0, depth = 0;
  double updateMilliseconds = 0;
  // Same bounded-distance importance estimate as the shader; useful for PDF
  // validation.
  static double weight(const LightNode &node, const float position[3],
                       const float normal[3], double range = 0);
  static double falloff(double distanceSquared, double range);

private:
  uint32_t build(std::vector<uint32_t> &order, uint32_t begin, uint32_t end,
                 uint32_t level);
  void bounds(uint32_t index);
  std::vector<SphereLight> m_lights;
  std::vector<LightEmitter> m_emitters;
  std::vector<LightNode> m_nodes;
};
} // namespace remix::scene
