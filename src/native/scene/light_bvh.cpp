#include "scene/light_bvh.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>

namespace remix::scene {
  namespace {
    double powerScale(const SphereLight &light) {
      if (!light.triangle)
        return double(light.radius) * light.radius;
      double a[3], b[3];
      for (unsigned c = 0; c < 3; ++c) {
        a[c] = double(light.corners[1][c]) - light.corners[0][c];
        b[c] = double(light.corners[2][c]) - light.corners[0][c];
      }
      const double area =
          .5 * std::hypot(a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]);
      return area * light.triangle / (4 * 3.141592653589793);
    }
  } // namespace
  bool LightBvh::update(std::vector<SphereLight> lights) {
    const auto start = std::chrono::steady_clock::now();
    bool orderChanged = lights.size() != m_sourceKeys.size();
    for (size_t i = 0; !orderChanged && i < lights.size(); ++i)
      orderChanged = m_sourceKeys[i] != std::pair{lights[i].id, lights[i].triangle};
    if (orderChanged) {
      m_sourceKeys.clear();
      for (const auto &light : lights)
        m_sourceKeys.emplace_back(light.id, light.triangle);
      m_order.resize(lights.size());
      std::iota(m_order.begin(), m_order.end(), 0u);
      std::sort(m_order.begin(), m_order.end(), [&](uint32_t a, uint32_t b) {
        return lights[a].triangle != lights[b].triangle ? lights[a].triangle < lights[b].triangle
                                                        : lights[a].id < lights[b].id;
      });
    }
    std::vector<SphereLight> ordered;
    ordered.reserve(lights.size());
    for (auto i : m_order)
      ordered.push_back(lights[i]);
    lights = std::move(ordered);
    if (lights == m_lights) {
      updateMilliseconds =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
      return false;
    }
    bool rebuild = lights.size() != m_lights.size();
    if (!rebuild)
      for (size_t i = 0; i < lights.size(); ++i)
        rebuild |= lights[i].id != m_lights[i].id || lights[i].triangle != m_lights[i].triangle;
    bool geometryChanged = rebuild;
    for (size_t i = 0; !geometryChanged && i < lights.size(); ++i) {
      const auto &a = lights[i], &b = m_lights[i];
      geometryChanged = a.coneCos != b.coneCos || a.softness != b.softness || a.focus != b.focus ||
                        !std::equal(a.direction, a.direction + 3, b.direction) || a.radius != b.radius ||
                        !std::equal(a.position, a.position + 3, b.position);
      for (unsigned v = 0; v < 3; ++v)
        geometryChanged |= !std::equal(a.corners[v], a.corners[v] + 3, b.corners[v]);
    }
    if (geometryChanged)
      ++visibilityGeneration;
    if (geometryChanged) {
      m_powerScale.resize(lights.size());
      for (size_t i = 0; i < lights.size(); ++i)
        m_powerScale[i] = powerScale(lights[i]);
    }
    // Tiny animated-emission fluctuations still update the GPU light data, but
    // must not keep every local-light filter/cache permanently in fast response.
    // Compare against the last significant snapshot, not the preceding frame,
    // so gradual ramps eventually cross the threshold too. A light that moved,
    // reshaped or switched on/off counts with its whole power: one animated or
    // carried light among many (a blinking siren, a bot's flashlight) must not
    // reset the local denoiser every frame, while a dominant one still does.
    bool significant = rebuild || lights.size() != m_temporalLights.size();
    double changed = 0, totalOldEnergy = 0, totalNewEnergy = 0;
    for (size_t i = 0; !significant && i < lights.size(); ++i) {
      const auto &a = lights[i], &b = m_temporalLights[i];
      double difference = 0, oldEnergy = 0, newEnergy = 0;
      for (unsigned c = 0; c < 3; ++c) {
        difference += std::abs(double(a.radiance[c]) - b.radiance[c]);
        oldEnergy += b.radiance[c];
        newEnergy += a.radiance[c];
      }
      bool moved = a.coneCos != b.coneCos || a.softness != b.softness || a.focus != b.focus ||
                   !std::equal(a.direction, a.direction + 3, b.direction) || a.radius != b.radius ||
                   !std::equal(a.position, a.position + 3, b.position);
      for (unsigned v = 0; v < 3; ++v)
        moved |= !std::equal(a.corners[v], a.corners[v] + 3, b.corners[v]);
      moved |= (oldEnergy == 0) != (newEnergy == 0);
      const double scale = m_powerScale[i];
      changed += (moved ? std::max(oldEnergy, newEnergy) : difference) * scale;
      totalOldEnergy += oldEnergy * scale;
      totalNewEnergy += newEnergy * scale;
    }
    // This is a scene-wide fast-response switch, not a per-emitter filter.
    // Small/local changes use NRD's ordinary history and anti-lag, rather
    // than forcing unrelated torches back to two accumulated frames.
    significant |= changed > .25 * std::max(totalOldEnergy, totalNewEnergy);
    if (significant) {
      ++lightingGeneration;
      m_temporalLights = lights;
    }
    m_emitters.resize(lights.size());
    m_dirty.assign(m_nodes.size(), geometryChanged);
    active = 0;
    for (size_t i = 0; i < lights.size(); ++i) {
      const auto &light = lights[i];
      active += light.radiance[0] > 0 || light.radiance[1] > 0 || light.radiance[2] > 0;
      if (!geometryChanged && light == m_lights[i])
        continue;
      if (!rebuild)
        m_dirty[m_leaves[i]] = 1;
      auto &emitter = m_emitters[i];
      std::copy_n(light.position, 3, emitter.positionRadius);
      emitter.positionRadius[3] = light.radius;
      std::copy_n(light.radiance, 3, emitter.radiance);
      emitter.radiance[3] = float(light.triangle);
      std::copy_n(light.direction, 3, emitter.directionCone);
      emitter.directionCone[3] = light.coneCos;
      emitter.shaping[0] = light.softness;
      emitter.shaping[1] = light.focus;
      for (unsigned v = 0; v < 3; ++v)
        std::copy_n(light.corners[v], 3, emitter.corners[v]);
    }
    m_lights = std::move(lights);
    if (rebuild) {
      m_nodes.clear();
      m_parents.clear();
      m_leaves.resize(m_lights.size());
      depth = 0;
      if (!m_lights.empty()) {
        m_nodes.reserve(2 * m_lights.size() - 1);
        std::vector<uint32_t> order(m_lights.size());
        for (uint32_t i = 0; i < order.size(); ++i)
          order[i] = i;
        build(order, 0, uint32_t(order.size()), 1);
      }
      ++builds;
    } else {
      for (size_t i = m_nodes.size(); i-- > 0;)
        if (m_dirty[i]) {
          bounds(uint32_t(i), geometryChanged);
          if (i)
            m_dirty[m_parents[i]] = 1;
        }
      ++refits;
    }
    ++generation;
    updateMilliseconds =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    return true;
  }

  // Nodes are laid out with both children of a node adjacent: the descent in
  // light_bvh.glsl reads both at every level, so one memory neighborhood per
  // level instead of a far jump to every right subtree.
  uint32_t LightBvh::build(std::vector<uint32_t> &order, uint32_t begin, uint32_t end, uint32_t level) {
    const uint32_t index = uint32_t(m_nodes.size());
    m_nodes.emplace_back();
    m_parents.push_back(0);
    buildInto(index, order, begin, end, level);
    return index;
  }

  void LightBvh::buildInto(uint32_t index, std::vector<uint32_t> &order, uint32_t begin, uint32_t end,
                           uint32_t level) {
    depth = std::max(depth, level);
    m_nodes[index].links[3] = end - begin;
    if (end - begin == 1) {
      m_nodes[index].links[2] = order[begin];
      m_leaves[order[begin]] = index;
    } else {
      float minimum[3], maximum[3];
      std::fill_n(minimum, 3, std::numeric_limits<float>::max());
      std::fill_n(maximum, 3, std::numeric_limits<float>::lowest());
      for (uint32_t i = begin; i < end; ++i)
        for (unsigned c = 0; c < 3; ++c) {
          minimum[c] = std::min(minimum[c], m_lights[order[i]].position[c]);
          maximum[c] = std::max(maximum[c], m_lights[order[i]].position[c]);
        }
      unsigned axis = 0;
      for (unsigned c = 1; c < 3; ++c)
        if (maximum[c] - minimum[c] > maximum[axis] - minimum[axis])
          axis = c;
      const uint32_t middle = begin + (end - begin) / 2;
      std::nth_element(order.begin() + begin, order.begin() + middle, order.begin() + end,
                       [&](uint32_t a, uint32_t b) {
                         return m_lights[a].position[axis] != m_lights[b].position[axis]
                                    ? m_lights[a].position[axis] < m_lights[b].position[axis]
                                    : a < b;
                       });
      const uint32_t left = uint32_t(m_nodes.size()), right = left + 1;
      m_nodes.emplace_back();
      m_nodes.emplace_back();
      m_parents.push_back(index);
      m_parents.push_back(index);
      m_nodes[index].links[0] = left;
      m_nodes[index].links[1] = right;
      buildInto(left, order, begin, middle, level + 1);
      buildInto(right, order, middle, end, level + 1);
    }
    bounds(index);
  }

  void LightBvh::bounds(uint32_t index, bool geometry) {
    auto &node = m_nodes[index];
    if (node.links[3] == 1) {
      const auto &light = m_lights[node.links[2]];
      if (geometry) {
        for (unsigned c = 0; c < 3; ++c) {
          node.minimumPower[c] = light.position[c] - light.radius;
          node.maximum[c] = light.position[c] + light.radius;
        }
        node.maximum[3] = light.radius * light.radius;
      }
      // Common 4*pi*pi cancels in every selection probability.
      node.minimumPower[3] =
          float(m_powerScale[node.links[2]] *
                (.2126 * light.radiance[0] + .7152 * light.radiance[1] + .0722 * light.radiance[2]));
    } else {
      const auto &a = m_nodes[node.links[0]], &b = m_nodes[node.links[1]];
      if (a.minimumPower[3] == 0 || b.minimumPower[3] == 0) {
        // Keep zero-power slots for cheap refits without expanding the live
        // influence bounds around distant unlit redstone torches.
        const auto &live = a.minimumPower[3] > 0 ? a : b;
        std::copy_n(live.minimumPower, 3, node.minimumPower);
        std::copy_n(live.maximum, 4, node.maximum);
      } else {
        for (unsigned c = 0; c < 3; ++c) {
          node.minimumPower[c] = std::min(a.minimumPower[c], b.minimumPower[c]);
          node.maximum[c] = std::max(a.maximum[c], b.maximum[c]);
        }
        node.maximum[3] = std::max(a.maximum[3], b.maximum[3]);
      }
      node.minimumPower[3] = a.minimumPower[3] + b.minimumPower[3];
    }
  }

  double LightBvh::falloff(double distanceSquared, double range) {
    if (range <= 0)
      return 1;
    const auto t = std::clamp((distanceSquared / (range * range) - .5625) * (16. / 7.), 0., 1.);
    return (1 - t) * (1 - t) * (1 + 2 * t);
  }

  double LightBvh::weight(const LightNode &node, const float position[3], const float normal[3],
                          double range) {
    double distance2 = 0, radius2 = 0, toward = 0, nearest2 = 0;
    for (unsigned c = 0; c < 3; ++c) {
      const double half = .5 * (node.maximum[c] - node.minimumPower[c]);
      const double delta = .5 * (node.maximum[c] + node.minimumPower[c]) - position[c];
      distance2 += delta * delta;
      radius2 += half * half;
      toward += delta * normal[c];
      const double nearest = std::max(std::abs(delta) - half, 0.);
      nearest2 += nearest * nearest;
    }
    // Cull only when the entire enclosing sphere is below the receiving plane.
    if (toward + std::sqrt(radius2) <= 0)
      return 0;
    const double cosine =
        std::min(1., (toward + std::sqrt(radius2)) / std::sqrt(std::max({distance2, radius2, 1.e-12})));
    return falloff(nearest2, range) * node.minimumPower[3] * cosine /
           std::max({nearest2, .01 * radius2, double(node.maximum[3]), 1.e-12});
  }
} // namespace remix::scene
