#include "scene/light_bvh.h"
#include "scene/resources.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <remix/remix_native_ext.h>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
} // namespace
int main() {
  using namespace remix::scene;
  require(LightBvh::falloff(1.e12, 0) == 1 && LightBvh::falloff(144, 16) == 1 &&
              LightBvh::falloff(256, 16) == 0 && LightBvh::falloff(400, 16) == 0,
          "Influence bound changed inner light or failed to reach black");
  double previousFalloff = 1;
  for (unsigned i = 0; i <= 1000; ++i) {
    const double distance = 12 + i * .004;
    const double fade = LightBvh::falloff(distance * distance, 16);
    require(fade >= 0 && fade <= previousFalloff, "Influence fade is not monotonic");
    previousFalloff = fade;
  }
  require(1 - LightBvh::falloff(12.001 * 12.001, 16) < 1.e-6 &&
              LightBvh::falloff(15.999 * 15.999, 16) < 1.e-6,
          "Influence fade has an endpoint discontinuity");
  {
    SphereLight live{}, unlit{};
    live.id = 1; live.radius = .08f; live.radiance[0] = 1;
    unlit.id = 2; unlit.radius = .08f; unlit.position[0] = 100;
    LightBvh bounds;
    bounds.update({live, unlit});
    require(bounds.nodes()[0].maximum[0] < .1f,
            "Unlit slots expanded the live influence bound");
    unlit.radiance[0] = 1;
    bounds.update({live, unlit});
    require(bounds.builds == 1 && bounds.refits == 1 && bounds.nodes()[0].maximum[0] > 100,
            "Reactivated emitter did not restore its bound by refitting");
  }
  remix::NativeResources resources;
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.radius = .08f;
  remixapi_LightInfo info{};
  info.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  info.pNext = &sphere;
  info.hash = 10;
  info.radiance = {200, 110, 44};
  remixapi_LightHandle handle{};
  resources.createLight(info, handle);
  resources.drawLight(handle);
  resources.drawLight(handle);
  auto accepted = resources.sphereLights();
  require(accepted.size() == 1, "Repeated draw duplicated emitter power");
  auto rejected = [&] {
    bool failed = false;
    try {
      resources.updateLight(handle, info);
    } catch (...) {
      failed = true;
    }
    require(failed && resources.sphereLights() == accepted,
            "Malformed sphere modified accepted state");
  };
  sphere.radius = -.1f;
  rejected();
  sphere.radius = std::numeric_limits<float>::infinity();
  rejected();
  sphere.radius = .08f;
  sphere.position.x = std::numeric_limits<float>::quiet_NaN();
  rejected();
  sphere.position.x = 0;
  info.radiance.y = -1;
  rejected();
  info.radiance.y = 110;
  sphere.shaping_hasvalue = 1;
  rejected();
  sphere.shaping_hasvalue = 0;
  sphere.radius = 0;
  resources.updateLight(handle, info);
  require(resources.sphereLights().empty(), "Zero-radius compatibility placeholder emitted light");
  sphere.radius = .08f;
  resources.updateLight(handle, info);
  require(resources.sphereLights() == accepted, "Finite-radius update did not restore the emitter");
  resources.finishFrame();
  require(resources.sphereLights().empty(),
          "Lights survived an absent submission");
  resources.drawLight(handle);
  resources.destroyLight(handle);
  require(resources.sphereLights().empty(),
          "Destroyed light survived draw selection");

  for (unsigned count : {1u, 100u, 1000u, 5000u}) {
    std::vector<SphereLight> lights;
    for (unsigned i = 0; i < count; ++i) {
      SphereLight light;
      light.id = i + 1;
      light.radius = .08f;
      light.position[0] = float(i % 71) - 35;
      light.position[1] = float(i / 71) - 35;
      light.position[2] = 4;
      light.radiance[0] = 1 + i % 13;
      lights.push_back(light);
    }
    LightBvh bvh;
    require(bvh.update(lights), "Initial BVH not built");
    require(bvh.active == count && bvh.nodes().size() == 2 * count - 1 &&
                bvh.depth <= unsigned(std::ceil(std::log2(count))) + 1,
            "Unbounded BVH depth or wrong membership");
    std::reverse(lights.begin(), lights.end());
    require(!bvh.update(lights) && bvh.builds == 1,
            "Unchanged reordered lights rebuilt BVH");
    const float p[] = {0, 0, 0}, n[] = {0, 0, 1};
    std::vector<double> pdf(count);
    std::function<void(uint32_t, double)> visit = [&](uint32_t node,
                                                      double probability) {
      const auto &value = bvh.nodes()[node];
      if (value.links[3] == 1) {
        pdf[value.links[2]] = probability;
        return;
      }
      const auto a = LightBvh::weight(bvh.nodes()[value.links[0]], p, n);
      const auto b = LightBvh::weight(bvh.nodes()[value.links[1]], p, n);
      const auto split = std::clamp(a / (a + b), 1.e-6, 1. - 1.e-6);
      visit(value.links[0], probability * split);
      visit(value.links[1], probability * (1 - split));
    };
    visit(0, 1);
    // Any leaf with positive physical attenuation must keep nonzero sampling
    // support at every enclosing node, including nodes near a range boundary.
    std::function<void(uint32_t, bool)> bounded = [&](uint32_t node, bool supported) {
      const auto &value = bvh.nodes()[node];
      supported &= LightBvh::weight(value, p, n, 16) > 0;
      if (value.links[3] == 1) {
        const auto &emitter = bvh.emitters()[value.links[2]];
        double d2 = 0;
        for (unsigned c = 0; c < 3; ++c)
          d2 += double(emitter.positionRadius[c]) * emitter.positionRadius[c];
        require(LightBvh::falloff(d2, 16) == 0 || supported,
                "Influence bounds culled a contributing emitter");
      } else {
        bounded(value.links[0], supported);
        bounded(value.links[1], supported);
      }
    };
    bounded(0, true);
    double sum = 0;
    for (unsigned i = 0; i < count; ++i) {
      require(pdf[i] > 0 && std::isfinite(pdf[i]),
              "A contributing light has zero selection PDF");
      sum += pdf[i];
    }
    require(std::abs(sum - 1) < 1.e-10, "BVH traversal did not reach every "
                                        "emitter with normalized probability");
    for (auto &light : lights)
      light.radiance[0] *= .5f;
    require(bvh.update(lights) && bvh.refits == 1 && bvh.builds == 1,
            "Power change rebuilt topology");
    for (auto &light : lights)
      light.position[0] += 128;
    require(bvh.update(lights) && bvh.refits == 2,
            "Origin shift did not refit bounds");
    for (auto &light : lights)
      light.radiance[0] = 0;
    bvh.update(lights);
    require(!bvh.active && bvh.nodes()[0].minimumPower[3] == 0,
            "Zero-power slots still contribute");
    bvh.update({});
    require(bvh.nodes().empty() && bvh.emitters().empty(),
            "Unloaded lights remained in hierarchy");
    std::printf("%u lights: balanced build, exact PDF sum, power/origin refit, "
                "removal passed\n",
                count);
  }
  // A receiver inside a volume of lights must not almost exclusively sample
  // whichever internal bounding box contains it. Measure the full discrete
  // estimator against the analytic sphere integral, before visibility or NRD.
  {
    std::vector<SphereLight> lights;
    for (int x = -5; x <= 5; ++x)
      for (int y = -5; y <= 5; ++y)
        for (int z = -5; z <= 5; ++z) {
          SphereLight light{};
          light.id = lights.size() + 1;
          light.radius = .08f;
          light.position[0] = 2 * x + .5f;
          light.position[1] = 2 * y + .75f;
          light.position[2] = 2 * z + .25f;
          light.radiance[0] = 200;
          light.radiance[1] = 110;
          light.radiance[2] = 44;
          lights.push_back(light);
        }
    LightBvh bvh;
    bvh.update(lights);
    for (float offset : {0.f, .5f, 1.f, 2.f}) {
      const float p[] = {offset, 0, 0}, n[] = {0, 0, 1};
      std::vector<double> pdf(lights.size());
      std::function<void(uint32_t, double)> visit = [&](uint32_t i,
                                                        double probability) {
        const auto &node = bvh.nodes()[i];
        if (node.links[3] == 1) {
          pdf[node.links[2]] = probability;
          return;
        }
        const double a = LightBvh::weight(bvh.nodes()[node.links[0]], p, n, 16);
        const double b = LightBvh::weight(bvh.nodes()[node.links[1]], p, n, 16);
        const double split = a == 0 ? 0
                             : b == 0
                                 ? 1
                                 : std::clamp(a / (a + b), 1.e-6, 1. - 1.e-6);
        visit(node.links[0], probability * split);
        visit(node.links[1], probability * (1 - split));
      };
      visit(0, 1);
      double reference = 0, expectation = 0, secondMoment = 0, maximum = 0,
             totalPdf = 0;
      for (size_t i = 0; i < lights.size(); ++i) {
        const auto &light = lights[i];
        double distance2 = 0;
        for (unsigned c = 0; c < 3; ++c)
          distance2 += std::pow(double(light.position[c]) - p[c], 2);
        // These spheres are entirely above or below the receiving plane.
        const double contribution =
            (.2126 * 200 + .7152 * 110 + .0722 * 44) * double(light.radius) *
            light.radius * std::max(light.position[2], 0.f) /
            std::pow(distance2, 1.5) * LightBvh::falloff(distance2, 16);
        totalPdf += pdf[i];
        reference += contribution;
        if (contribution == 0)
          continue;
        require(pdf[i] > 0, "Interior receiver lost a contributing light");
        const double estimate = contribution / pdf[i];
        expectation += pdf[i] * estimate;
        secondMoment += pdf[i] * estimate * estimate;
        maximum = std::max(maximum, estimate);
      }
      require(std::abs(totalPdf - 1) < 1.e-10 &&
                  std::abs(expectation - reference) < 1.e-10,
              "Interior light selection changed the sphere integral");
      const double relativeDeviation =
          std::sqrt(secondMoment - reference * reference) / reference;
      require(relativeDeviation < 4 && maximum / reference < 80,
              "Interior light cluster produced excessive inverse-PDF outliers");
    }
    std::puts(
        "1331 torches: interior receiver energy and selection variance passed");
  }
}
