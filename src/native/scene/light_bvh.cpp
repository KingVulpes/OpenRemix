#include "scene/light_bvh.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

namespace remix::scene {
bool LightBvh::update(std::vector<SphereLight> lights) {
  const auto start = std::chrono::steady_clock::now();
  std::sort(lights.begin(), lights.end(),
            [](const auto &a, const auto &b) { return a.id < b.id; });
  if (lights == m_lights) {
    updateMilliseconds = std::chrono::duration<double, std::milli>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    return false;
  }
  bool rebuild = lights.size() != m_lights.size();
  if (!rebuild)
    for (size_t i = 0; i < lights.size(); ++i)
      rebuild |= lights[i].id != m_lights[i].id;
  m_lights = std::move(lights);
  m_emitters.resize(m_lights.size());
  active = 0;
  for (size_t i = 0; i < m_lights.size(); ++i) {
    const auto &light = m_lights[i];
    auto &emitter = m_emitters[i];
    std::copy_n(light.position, 3, emitter.positionRadius);
    emitter.positionRadius[3] = light.radius;
    std::copy_n(light.radiance, 3, emitter.radiance);
    active +=
        light.radiance[0] > 0 || light.radiance[1] > 0 || light.radiance[2] > 0;
  }
  if (rebuild) {
    m_nodes.clear();
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
      bounds(uint32_t(i));
    ++refits;
  }
  ++generation;
  updateMilliseconds = std::chrono::duration<double, std::milli>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  return true;
}

uint32_t LightBvh::build(std::vector<uint32_t> &order, uint32_t begin,
                         uint32_t end, uint32_t level) {
  const uint32_t index = uint32_t(m_nodes.size());
  m_nodes.emplace_back();
  depth = std::max(depth, level);
  m_nodes[index].links[3] = end - begin;
  if (end - begin == 1) {
    m_nodes[index].links[2] = order[begin];
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
    std::nth_element(
        order.begin() + begin, order.begin() + middle, order.begin() + end,
        [&](uint32_t a, uint32_t b) {
          return m_lights[a].position[axis] != m_lights[b].position[axis]
                     ? m_lights[a].position[axis] < m_lights[b].position[axis]
                     : a < b;
        });
    const auto left = build(order, begin, middle, level + 1);
    const auto right = build(order, middle, end, level + 1);
    m_nodes[index].links[0] = left;
    m_nodes[index].links[1] = right;
  }
  bounds(index);
  return index;
}

void LightBvh::bounds(uint32_t index) {
  auto &node = m_nodes[index];
  if (node.links[3] == 1) {
    const auto &light = m_lights[node.links[2]];
    for (unsigned c = 0; c < 3; ++c) {
      node.minimumPower[c] = light.position[c] - light.radius;
      node.maximum[c] = light.position[c] + light.radius;
    }
    node.maximum[3] = light.radius * light.radius;
    // Common 4*pi*pi cancels in every selection probability.
    node.minimumPower[3] =
        float(double(light.radius) * light.radius *
              (.2126 * light.radiance[0] + .7152 * light.radiance[1] +
               .0722 * light.radiance[2]));
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

double LightBvh::weight(const LightNode &node, const float position[3],
                        const float normal[3], double range) {
  double distance2 = 0, radius2 = 0, toward = 0, nearest2 = 0;
  for (unsigned c = 0; c < 3; ++c) {
    const double half = .5 * (node.maximum[c] - node.minimumPower[c]);
    const double delta =
        .5 * (node.maximum[c] + node.minimumPower[c]) - position[c];
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
      std::min(1., (toward + std::sqrt(radius2)) /
                       std::sqrt(std::max({distance2, radius2, 1.e-12})));
  return falloff(nearest2, range) * node.minimumPower[3] * cosine /
         std::max({nearest2, .01 * radius2, double(node.maximum[3]), 1.e-12});
}
} // namespace remix::scene
