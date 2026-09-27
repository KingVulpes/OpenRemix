#include "scene/emission.h"
#include <algorithm>
#include <cmath>

namespace remix::scene {
  namespace {
    // Texture channels have only 256 possible inputs. Animated texture snapshots
    // invalidate the reduction cache, but not this conversion or the quadrature.
    const auto linearChannels = [] {
      std::array<std::array<float, 256>, 2> values{};
      for (unsigned i = 0; i < 256; ++i) {
        const float value = i / 255.f;
        values[0][i] = value;
        values[1][i] = value <= .04045f ? value / 12.92f : std::pow((value + .055f) / 1.055f, 2.4f);
      }
      return values;
    }();
    const auto emissionSamples = [] {
      std::array<std::array<float, 3>, 256> weights{};
      for (unsigned y = 0; y < 16; ++y)
        for (unsigned x = 0; x < 16; ++x) {
          const float a = std::sqrt((x + .5f) / 16), b = (y + .5f) / 16;
          weights[y * 16 + x] = {1 - a, a * (1 - b), a * b};
        }
      return weights;
    }();
  } // namespace
  std::shared_ptr<const SurfaceEmissionCache::Reduction>
  SurfaceEmissionCache::reduce(const Draw &draw, const std::array<float, 4> &uv) {
    const auto &m = *draw.material;
    const Key key{
        {m.emission[0], m.emission[1], m.emission[2]}, uv, m.wrapU, m.wrapV, m.rows, m.columns, m.fps};
    auto &cache = surfaces[draw.surface.get()];
    if (cache.owner.lock() != draw.surface)
      cache = {draw.surface, {}};
    if (const auto found = cache.entries.find(key); found != cache.entries.end()) {
      const auto &entry = found->second;
      if (entry.textured == bool(m.emissionTexture) && entry.texture.lock() == m.emissionTexture)
        return entry.reduction;
    }
    auto result = std::make_shared<Reduction>();
    for (uint32_t triangle = 0; triangle < draw.surface->indices.size() / 3; ++triangle) {
      const auto radiance = triangleEmission(draw, triangle, uv.data());
      if (radiance[0] > 0 || radiance[1] > 0 || radiance[2] > 0)
        result->push_back({triangle, radiance, m.emissionTexture ? emissionHistoryKey(draw, triangle) : 0});
    }
    // Bound sprite/setting variants without retaining old surfaces or textures.
    if (cache.entries.size() >= 8)
      cache.entries.clear();
    cache.entries.insert_or_assign(key, Entry{m.emissionTexture, bool(m.emissionTexture), result});
    return result;
  }

  void SurfaceEmissionCache::prune() {
    std::erase_if(surfaces, [](const auto &entry) { return entry.second.owner.expired(); });
  }

  uint64_t emissionHistoryKey(const Draw &draw, uint32_t triangle) {
    const auto &m = *draw.material;
    std::array<uint8_t, 64> bytes{};
    size_t size = 0;
    auto add = [&](const auto &value) {
      auto p = reinterpret_cast<const uint8_t *>(&value);
      std::copy_n(p, sizeof(value), bytes.data() + size);
      size += sizeof(value);
    };
    add(m.emission);
    add(m.wrapU);
    add(m.wrapV);
    add(m.rows);
    add(m.columns);
    add(m.fps);
    if (m.emissionTexture) {
      add(m.emissionTexture->pixels.width);
      add(m.emissionTexture->pixels.height);
      add(m.emissionTexture->pixels.srgb);
    }
    for (unsigned i = 0; i < 3; ++i)
      add(draw.surface->vertices[draw.surface->indices[triangle * 3 + i]].uv);
    return hashBytes(bytes.data(), size);
  }
  void EmissionHistory::begin(uint64_t nextWorld, double seconds, uint32_t nextMode) {
    if (nextWorld != world || nextMode != mode || seconds < time)
      entries.clear();
    alpha = -std::expm1(-std::max(0., seconds - time) / .5);
    world = nextWorld;
    time = seconds;
    mode = nextMode;
    ++sequence;
    rawChange = filteredChange = 0;
  }
  void EmissionHistory::apply(SphereLight &light, uint64_t layout) {
    if (!mode || !light.triangle)
      return;
    if (!(light.radiance[0] > 0 || light.radiance[1] > 0 || light.radiance[2] > 0)) {
      entries.erase(light.id);
      return;
    }
    // World-space geometry changes invalidate correspondence independently of
    // animation. Hash only initialized xyz corners, never structure padding.
    layout ^= hashBytes(light.corners, sizeof(light.corners));
    auto [it, inserted] = entries.try_emplace(light.id);
    auto &entry = it->second;
    if (inserted || entry.layout != layout)
      for (unsigned c = 0; c < 3; ++c)
        entry.raw[c] = entry.filtered[c] = light.radiance[c];
    entry.layout = layout;
    entry.seen = sequence;
    for (unsigned c = 0; c < 3; ++c) {
      const float raw = light.radiance[c], previous = entry.filtered[c];
      rawChange += std::abs(raw - entry.raw[c]);
      if (mode == 1) {
        entry.filtered[c] += float(alpha * (double(raw) - entry.filtered[c]));
        if (std::abs(entry.filtered[c] - raw) <= 1e-5f * std::max(1.f, raw))
          entry.filtered[c] = raw;
      }
      filteredChange += std::abs(entry.filtered[c] - previous);
      entry.raw[c] = raw;
      light.radiance[c] = entry.filtered[c];
    }
  }
  void EmissionHistory::end() {
    std::erase_if(entries, [&](const auto &item) { return item.second.seen != sequence; });
  }
  std::array<float, 3> triangleEmission(const Draw &draw, uint32_t triangle, const float *transform) {
    const auto &m = *draw.material;
    if (m.emission[0] == 0 && m.emission[1] == 0 && m.emission[2] == 0)
      return {};
    std::array<float, 3> result{};
    if (!m.emissionTexture) {
      std::copy_n(m.emission, 3, result.begin());
      return result;
    }
    const auto &s = *draw.surface;
    float uv[3][2];
    for (unsigned i = 0; i < 3; ++i)
      for (unsigned c = 0; c < 2; ++c)
        uv[i][c] = s.vertices[s.indices[triangle * 3 + i]].uv[c] * transform[c] + transform[c + 2];
    auto key = hashBytes(uv, sizeof(uv));
    if (auto it = m.emissionCache.find(key); it != m.emissionCache.end())
      return it->second;
    const auto &p = m.emissionTexture->pixels;
    auto coord = [](float value, uint32_t size, uint8_t wrap) {
      if (wrap <= 1)
        value -= std::floor(value);
      else if (wrap == 2) {
        value = std::fmod(std::abs(value), 2.f);
        if (value > 1)
          value = 2 - value;
      }
      return std::min(size - 1, uint32_t(std::clamp(value, 0.f, 1.f) * size));
    };
    // 16x16 stratified area samples, only on cache misses. Black texels count in
    // the average so small masks keep their coverage and do not gain energy.
    const auto &channels = linearChannels[p.srgb];
    const auto &level = p.hdrMips.empty() ? p.rgba()[0] : p.mips[0];
    for (const auto &w : emissionSamples) {
      float u = 0, v = 0;
      for (unsigned i = 0; i < 3; ++i) {
        u += uv[i][0] * w[i];
        v += uv[i][1] * w[i];
      }
      if ((m.wrapU == 4 && (u < 0 || u > 1)) || (m.wrapV == 4 && (v < 0 || v > 1)))
        continue;
      const auto index = 4 * (size_t(coord(v, p.height, m.wrapV)) * p.width + coord(u, p.width, m.wrapU));
      for (unsigned c = 0; c < 3; ++c) {
        const float value = p.hdrMips.empty() ? channels[level[index + c]] : p.hdrMips[0][index + c];
        result[c] += value * m.emission[c] / 256;
      }
    }
    // Bound caches for arbitrary animated UVs; eviction affects only CPU work.
    if (m.emissionCache.size() >= 65536)
      m.emissionCache.clear();
    m.emissionCache.emplace(key, result);
    return result;
  }

  SphereLight triangleEmitter(const Draw &draw, uint32_t triangle, const std::array<float, 3> &radiance) {
    SphereLight light;
    const uint64_t identity[] = {draw.instanceId ? draw.instanceId
                                                 : uint64_t(reinterpret_cast<uintptr_t>(draw.mesh.get())),
                                 draw.instanceIndex, draw.surfaceIndex, triangle};
    light.id = hashBytes(identity, sizeof(identity));
    light.triangle = draw.doubleSided ? 2 : 1;
    std::copy(radiance.begin(), radiance.end(), light.radiance);
    const auto &s = *draw.surface;
    for (unsigned i = 0; i < 3; ++i) {
      auto index = s.indices[triangle * 3 + i];
      auto vertex = s.vertices[index];
      if (s.bonesPerVertex && draw.bones) {
        auto original = vertex;
        std::fill_n(vertex.position, 3, 0.f);
        for (unsigned b = 0; b < s.bonesPerVertex; ++b) {
          auto weight = index * s.bonesPerVertex + b;
          const auto &m = (*draw.bones)[s.bones[weight]];
          for (unsigned c = 0; c < 3; ++c)
            vertex.position[c] +=
                s.weights[weight] * (m[c] * original.position[0] + m[4 + c] * original.position[1] +
                                     m[8 + c] * original.position[2] + m[12 + c]);
        }
      }
      for (unsigned c = 0; c < 3; ++c) {
        const auto &m = draw.model;
        light.corners[i][c] = m[c] * vertex.position[0] + m[4 + c] * vertex.position[1] +
                              m[8 + c] * vertex.position[2] + m[12 + c];
        light.position[c] += light.corners[i][c] / 3;
      }
    }
    for (unsigned i = 0; i < 3; ++i)
      light.radius = std::max(light.radius, std::hypot(light.corners[i][0] - light.position[0],
                                                       light.corners[i][1] - light.position[1],
                                                       light.corners[i][2] - light.position[2]));
    return light;
  }
} // namespace remix::scene
