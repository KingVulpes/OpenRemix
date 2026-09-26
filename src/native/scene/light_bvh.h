#pragma once
#include "scene/types.h"
#include <utility>
#include <vector>

namespace remix::scene {
  // std430 records shared with light_bvh.glsl. Analytic emitters need no
  // geometry.
  struct LightEmitter {
    float positionRadius[4] = {};
    float radiance[4] = {};
    float corners[3][4] = {};
    float directionCone[4] = {}, shaping[4] = {};
  };
  struct LightNode {
    float minimumPower[4] = {};
    float maximum[4] = {};  // xyz bounds; w largest emitter radius squared
    uint32_t links[4] = {}; // left, right, emitter, leaf count
  };
  static_assert(sizeof(LightEmitter) == 112 && sizeof(LightNode) == 48);
  class LightBvh {
  public:
    bool update(std::vector<SphereLight> lights);
    const std::vector<LightEmitter> &emitters() const { return m_emitters; }
    const std::vector<LightNode> &nodes() const { return m_nodes; }
    uint64_t generation = 0, builds = 0, refits = 0;
    // Separate transport updates, scene-wide temporal response and the spatial
    // correspondence used by visibility guidance. Color animation changes only
    // transport unless its aggregate power change is significant.
    uint64_t lightingGeneration = 0, visibilityGeneration = 0;
    uint32_t active = 0, depth = 0;
    double updateMilliseconds = 0;
    // Same bounded-distance importance estimate as the shader; useful for PDF
    // validation.
    static double weight(const LightNode &node, const float position[3], const float normal[3],
                         double range = 0);
    static double falloff(double distanceSquared, double range);

  private:
    uint32_t build(std::vector<uint32_t> &order, uint32_t begin, uint32_t end, uint32_t level);
    void bounds(uint32_t index, bool geometry = true);
    // Input identity order and triangle areas survive radiance animation.
    std::vector<std::pair<uint64_t, uint32_t>> m_sourceKeys;
    std::vector<uint32_t> m_order, m_parents, m_leaves;
    std::vector<double> m_powerScale;
    std::vector<uint8_t> m_dirty;
    std::vector<SphereLight> m_lights;
    std::vector<SphereLight> m_temporalLights;
    std::vector<LightEmitter> m_emitters;
    std::vector<LightNode> m_nodes;
  };
} // namespace remix::scene
