#include "scene/trace_frame.h"
#include "scene/fixtures.h"
#include <cmath>

using namespace remix::scene;
using namespace remix::test;

int main() {
  SurfaceEmissionCache emissionCache;
  Draw draw;
  draw.instanceId = 5;
  draw.mesh = std::make_shared<Mesh>();
  draw.material = std::make_shared<Material>();
  draw.surface = std::make_shared<Surface>();
  draw.surface->vertices = {
      {{0, 0, 0}, {.1f, .1f}, {}}, {{1, 0, 0}, {.9f, .1f}, {}}, {{0, 1, 0}, {.1f, .9f}, {}}};
  draw.surface->indices = {0, 1, 2};
  std::fill_n(draw.material->emission, 3, 1.f);
  draw.material->columns = 2;
  draw.material->fps = 2;
  draw.material->emissionTexture = std::make_shared<Texture>();
  draw.material->emissionTexture->pixels = {2, 1, true, {{255, 0, 0, 255, 0, 255, 0, 255}}};

  const auto first = prepareTraceFrame({draw}, .1, true, emissionCache);
  const auto second = prepareTraceFrame({draw}, .6, true, emissionCache);
  const auto wrapped = prepareTraceFrame({draw}, 1.1, true, emissionCache);
  require(first.instances[0][0].animationFrame == 0 && second.instances[0][0].animationFrame == 1,
          "Sprite frame did not advance");
  require(first.emitters.size() == 1 && second.emitters.size() == 1 && wrapped.emitters.size() == 1 &&
              first.emitters[0].light.radiance[0] == 1 && first.emitters[0].light.radiance[1] == 0 &&
              second.emitters[0].light.radiance[0] == 0 && second.emitters[0].light.radiance[1] == 1 &&
              first.emitters[0].light == wrapped.emitters[0].light,
          "Sprite emission reduction does not match the staged UV frame");
  require(first.emitters[0].animated && first.emitters[0].layout == second.emitters[0].layout &&
              first.emitters[0].light.id == second.emitters[0].light.id,
          "Animation changed emitter correspondence");
  require(first.emitters[0].light.radiance[0] == 1 && second.emitters[0].light.radiance[1] == 1,
          "Preparing another frame mutated pending lighting");

  auto legacy = draw;
  legacy.instanceId = 0;
  auto explicitId = draw;
  explicitId.instanceId = UINT64_MAX;
  auto nextSurface = draw;
  nextSurface.surfaceIndex = 1;
  const auto ordered = prepareTraceFrame({explicitId, legacy, draw, nextSurface}, 0, false, emissionCache);
  require(ordered.instances.size() == 3 && ordered.instances[0].size() == 2 &&
              ordered.instances[0][0].draw.instanceId == 5 &&
              ordered.instances[1][0].draw.instanceId == UINT64_MAX - 1 &&
              !ordered.persistentIds.count(UINT64_MAX - 1) && ordered.persistentIds.count(UINT64_MAX),
          "Grouping, ordering or legacy identities changed");
  require(ordered.emitters[0].light.id != ordered.emitters[1].light.id && !ordered.emitters[0].animated,
          "Surfaces share emitter identity or disabled filtering is staged");
  rejected([&] { prepareTraceFrame({draw, draw}, 0, false, emissionCache); });
  rejected([&] { prepareTraceFrame({nextSurface}, 0, false, emissionCache); });

  auto singular = draw;
  singular.instanceId = 6;
  singular.model[0] = 0;
  auto overlay = draw;
  overlay.classification = 2;
  auto sky = draw;
  sky.camera = REMIXAPI_CAMERA_TYPE_SKY;
  const auto empty = prepareTraceFrame({singular, overlay, sky}, 0, true, emissionCache);
  require(empty.instances.empty() && empty.emitters.empty(),
          "Untraced or singular geometry contributes emission");

  // Replacement snapshots must leave an already prepared frame intact.
  auto owner = draw.material;
  draw.material = std::make_shared<Material>(*owner);
  draw.material->emissionTexture = std::make_shared<Texture>();
  draw.material->emissionTexture->pixels = {2, 1, true, {{0, 0, 255, 255, 0, 0, 0, 255}}};
  draw.material->emissionCache.clear();
  const auto replaced = prepareTraceFrame({draw}, .1, true, emissionCache);
  require(replaced.emitters.size() == 1 && replaced.emitters[0].light.radiance[2] == 1 &&
              replaced.emitters[0].light.radiance[0] == 0 &&
              first.instances[0][0].draw.material == owner && first.emitters[0].light.radiance[0] == 1,
          "Texture replacement reused stale reduction or lost snapshot ownership");

  draw.surface = std::make_shared<Surface>(*draw.surface);
  draw.surface->bonesPerVertex = 1;
  draw.surface->weights = {1, 1, 1};
  draw.surface->bones = {0, 0, 0};
  draw.bones = std::make_shared<std::vector<Matrix>>(1, identity());
  (*draw.bones)[0][12] = 2;
  draw.model[12] = 3;
  draw.doubleSided = true;
  const auto skinned = prepareTraceFrame({draw}, .1, false, emissionCache);
  require(skinned.emitters[0].light.corners[0][0] == 5 && skinned.emitters[0].light.triangle == 2,
          "Prepared emitter lost skinning, world transform or sidedness");
  // Albedo animation and instance state must reuse reductions while producing
  // fresh world-space light geometry. Replacing emission/UV snapshots must not.
  const auto uv = skinned.instances[0][0].uv;
  const auto reduction = emissionCache.reduce(draw, uv);
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->texture = std::make_shared<Texture>();
  require(emissionCache.reduce(draw, uv) == reduction, "Unrelated albedo invalidated emission reduction");
  draw.model[12] = 7;
  draw.instanceId = 42;
  draw.doubleSided = false;
  draw.bones = std::make_shared<std::vector<Matrix>>(1, identity());
  (*draw.bones)[0][12] = 4;
  const auto moved = prepareTraceFrame({draw}, .1, true, emissionCache);
  require(moved.emitters[0].light.corners[0][0] == 11 && moved.emitters[0].light.triangle == 1 &&
              moved.emitters[0].light.id != skinned.emitters[0].light.id &&
              skinned.emitters[0].light.corners[0][0] == 5 && emissionCache.reduce(draw, uv) == reduction,
          "Cached reduction froze instance identity, transform, skinning or sidedness");
  auto compareFresh = [&](double seconds) {
    SurfaceEmissionCache fresh;
    const auto expected = prepareTraceFrame({draw}, seconds, true, fresh);
    const auto actual = prepareTraceFrame({draw}, seconds, true, emissionCache);
    require(actual.emitters.size() == expected.emitters.size(), "Cached emitter count differs from fresh reduction");
    for (size_t i = 0; i < actual.emitters.size(); ++i)
      require(actual.emitters[i].light == expected.emitters[i].light &&
                  actual.emitters[i].layout == expected.emitters[i].layout,
              "Cached emission differs from fresh reduction");
    return actual;
  };
  // A previously black sprite becomes luminous, without changing geometry.
  require(compareFresh(.6).emitters.empty(), "Black sprite entered light hierarchy");
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->emissionTexture = std::make_shared<Texture>();
  draw.material->emissionTexture->pixels = {2, 1, true, {{0,0,0,255,255,255,255,255}}};
  draw.material->emissionCache.clear();
  require(compareFresh(.6).emitters.size() == 1 && reduction->front().radiance[2] == 1,
          "Emission replacement reused black cache or mutated pending reduction");
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->emission[0] = 3;
  draw.material->emissionCache.clear();
  require(compareFresh(.6).emitters[0].light.radiance[0] == 3, "Changed emission constant reused stale radiance");
  draw.material = std::make_shared<Material>(*draw.material);
  draw.material->emissionTexture.reset();
  require(compareFresh(.6).emitters[0].light.radiance[0] == 3, "Constant emission inherited textured reduction");
  draw.surface = std::make_shared<Surface>(*draw.surface);
  draw.surface->indices.insert(draw.surface->indices.end(), {0, 2, 1});
  require(compareFresh(.6).emitters.size() == 2, "Replacement geometry reused old triangle list");
  std::weak_ptr<Surface> removed = draw.surface;
  draw.surface.reset();
  emissionCache.prune();
  require(removed.expired(), "Emission cache retained retired geometry");
  std::puts("CPU tracing snapshots, identities, animation, emitters and "
            "ownership passed");
}
