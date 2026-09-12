#include "scene/trace_frame.h"
#include "scene/emission.h"
#include <algorithm>
#include <cmath>
#include <tuple>

namespace remix::scene {
TraceFrame prepareTraceFrame(const std::vector<Draw> &draws, double seconds, bool filterEmission) {
  TraceFrame frame;
  frame.seconds = seconds;
  // Group whole mesh submissions, then sort persistent identities so caller
  // order is immaterial. Legacy identities must not collide with explicit IDs.
  for (const auto &draw : draws)
    if (draw.instanceId)
      frame.persistentIds.insert(draw.instanceId);
  uint64_t legacy = UINT64_MAX, legacyId = 0;
  for (const auto &draw : draws) {
    if (!draw.traced())
      continue;
    if (!draw.surfaceIndex)
      frame.instances.emplace_back();
    if (frame.instances.empty())
      throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    auto copy = draw;
    if (!draw.instanceId) {
      if (!draw.surfaceIndex) {
        while (frame.persistentIds.count(legacy))
          --legacy;
        legacyId = legacy--;
      }
      copy.instanceId = legacyId;
    }
    frame.instances.back().push_back({std::move(copy)});
  }
  std::sort(frame.instances.begin(), frame.instances.end(), [](const auto &a, const auto &b) {
    return std::tie(a[0].draw.classification, a[0].draw.instanceId, a[0].draw.instanceIndex) <
           std::tie(b[0].draw.classification, b[0].draw.instanceId, b[0].draw.instanceIndex);
  });
  for (size_t i = 1; i < frame.instances.size(); ++i) {
    const auto &a = frame.instances[i - 1][0].draw, &b = frame.instances[i][0].draw;
    if (a.instanceId == b.instanceId && a.instanceIndex == b.instanceIndex)
      throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  // Singular transforms cannot produce valid Vulkan instances or emitters.
  std::erase_if(frame.instances, [](const auto &instance) {
    const auto &m = instance[0].draw.model;
    const float det = m[0] * (m[5] * m[10] - m[9] * m[6]) - m[4] * (m[1] * m[10] - m[9] * m[2]) +
                      m[8] * (m[1] * m[6] - m[5] * m[2]);
    return std::abs(det) < 1e-12f;
  });
  for (auto &instance : frame.instances)
    for (auto &surface : instance) {
      const auto &draw = surface.draw;
      const auto &material = *draw.material;
      const auto animation =
          uint32_t(std::fmod(seconds * material.fps, uint32_t(material.rows) * material.columns));
      surface.animationFrame = animation;
      surface.uv = {1.f / material.columns, 1.f / material.rows,
                    float(animation % material.columns) / material.columns,
                    float(animation / material.columns) / material.rows};
      if (material.emission[0] <= 0 && material.emission[1] <= 0 && material.emission[2] <= 0)
        continue;
      for (uint32_t triangle = 0; triangle < draw.surface->indices.size() / 3; ++triangle) {
        const auto emission = triangleEmission(draw, triangle, surface.uv.data());
        frame.emissions.push_back({emission[0], emission[1], emission[2], 0});
        if (emission[0] > 0 || emission[1] > 0 || emission[2] > 0) {
          const bool animated = filterEmission && bool(material.emissionTexture);
          frame.emitters.push_back({triangleEmitter(draw, triangle, emission),
                                    animated ? emissionHistoryKey(draw, triangle) : 0, animated});
        }
      }
    }
  return frame;
}
} // namespace remix::scene
