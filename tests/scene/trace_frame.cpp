#include "scene/trace_frame.h"
#include "scene/fixtures.h"
#include <cmath>

using namespace remix::scene;
using namespace remix::test;

int main() {
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

  const auto first = prepareTraceFrame({draw}, .1, true);
  const auto second = prepareTraceFrame({draw}, .6, true);
  const auto wrapped = prepareTraceFrame({draw}, 1.1, true);
  require(first.instances[0][0].animationFrame == 0 && second.instances[0][0].animationFrame == 1,
          "Sprite frame did not advance");
  require(first.emissions.size() == 2 && first.emissions[0] == std::array<float, 4>{} &&
              first.emissions[1] == std::array<float, 4>{1, 0, 0, 0} &&
              second.emissions[1] == std::array<float, 4>{0, 1, 0, 0} &&
              first.emissions == wrapped.emissions,
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
  const auto ordered = prepareTraceFrame({explicitId, legacy, draw, nextSurface}, 0, false);
  require(ordered.instances.size() == 3 && ordered.instances[0].size() == 2 &&
              ordered.instances[0][0].draw.instanceId == 5 &&
              ordered.instances[1][0].draw.instanceId == UINT64_MAX - 1 &&
              !ordered.persistentIds.count(UINT64_MAX - 1) && ordered.persistentIds.count(UINT64_MAX),
          "Grouping, ordering or legacy identities changed");
  require(ordered.emitters[0].light.id != ordered.emitters[1].light.id && !ordered.emitters[0].animated,
          "Surfaces share emitter identity or disabled filtering is staged");
  rejected([&] { prepareTraceFrame({draw, draw}, 0, false); });
  rejected([&] { prepareTraceFrame({nextSurface}, 0, false); });

  auto singular = draw;
  singular.instanceId = 6;
  singular.model[0] = 0;
  auto overlay = draw;
  overlay.classification = 2;
  auto sky = draw;
  sky.camera = REMIXAPI_CAMERA_TYPE_SKY;
  const auto empty = prepareTraceFrame({singular, overlay, sky}, 0, true);
  require(empty.instances.empty() && empty.emitters.empty() && empty.emissions.size() == 1,
          "Untraced or singular geometry contributes emission");

  // Replacement snapshots must leave an already prepared frame intact.
  auto owner = draw.material;
  draw.material = std::make_shared<Material>(*owner);
  draw.material->emissionTexture = std::make_shared<Texture>();
  draw.material->emissionTexture->pixels = {2, 1, true, {{0, 0, 255, 255, 0, 0, 0, 255}}};
  draw.material->emissionCache.clear();
  const auto replaced = prepareTraceFrame({draw}, .1, true);
  require(replaced.emissions[1] == std::array<float, 4>{0, 0, 1, 0} &&
              first.instances[0][0].draw.material == owner && first.emissions[1][0] == 1,
          "Texture replacement reused stale reduction or lost snapshot ownership");

  draw.surface->bonesPerVertex = 1;
  draw.surface->weights = {1, 1, 1};
  draw.surface->bones = {0, 0, 0};
  draw.bones = std::make_shared<std::vector<Matrix>>(1, identity());
  (*draw.bones)[0][12] = 2;
  draw.model[12] = 3;
  draw.doubleSided = true;
  const auto skinned = prepareTraceFrame({draw}, .1, false);
  require(skinned.emitters[0].light.corners[0][0] == 5 && skinned.emitters[0].light.triangle == 2,
          "Prepared emitter lost skinning, world transform or sidedness");
  std::puts("CPU tracing snapshots, identities, animation, emitters and "
            "ownership passed");
}
