#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <vector>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
void require(bool value, const char *message) {
  if (!value) {
    std::fprintf(stderr, "%s\n", message);
    std::exit(1);
  }
}
void ok(remixapi_ErrorCode result) {
  require(!result, "Environment test API failure");
}
} // namespace
int main(int argc, char **argv) {
  require(argc == 2, "Supply screenshot directory");
  const auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = std::getenv("REMIX_TEST_LOCAL_PERF") ? 1280 : 128;
  startup.windowHeight = std::getenv("REMIX_TEST_LOCAL_PERF") ? 720 : 128;
#endif
  ok(api.Startup(&startup));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  if (!(caps.supportedBackends & 2)) {
    std::printf("Sky scenes unavailable: %s\n", caps.unavailableReason);
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.samples", "1"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bounces", "0"));
  require(api.SetConfigVariable("rtx.native.pathtrace.sky", "2") ==
              REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Invalid sky toggle accepted");
  ok(api.SetConfigVariable("rtx.native.vsync", "False"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.sharc", "0"));
  uint64_t next = 100;
  auto material = [&](remixapi_Float3D color, float alpha,
                      bool cutout = false) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = cutout ? 4 : 7; // GREATER .5
    opaque.alphaReferenceValue = 128;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = next++;
    remixapi_MaterialHandle handle{};
    ok(api.CreateMaterial(&info, &handle));
    return handle;
  };
  const auto white = material({1, 1, 1}, 1), black = material({0, 0, 0}, 1);
  const auto cutout = material({1, 1, 1}, 0, true),
             tint = material({.1f, 1, .2f}, .5f);
  auto quad = [&](int axis, float plane, float a0, float a1, float b0, float b1,
                  remixapi_MaterialHandle mat) {
    remixapi_HardcodedVertex vertices[4]{};
    for (int i = 0; i < 4; ++i) {
      vertices[i].position[axis] = plane;
      vertices[i].position[(axis + 1) % 3] = i == 0 || i == 3 ? a0 : a1;
      vertices[i].position[(axis + 2) % 3] = i < 2 ? b0 : b1;
      vertices[i].color = 0xffffffff;
    }
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = vertices;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = mat;
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = next++;
    mesh.surfaces_values = &surface;
    mesh.surfaces_count = 1;
    remixapi_MeshHandle handle{};
    ok(api.CreateMesh(&mesh, &handle));
    return handle;
  };
  const auto receiver = quad(2, .8f, -2, 2, -2, 2, white);
  const auto blocker = quad(0, .25f, -2, 2, -2, .7f, black);
  const auto holes = quad(0, .25f, -2, 2, -2, .7f, cutout);
  const auto glass = quad(0, .25f, -2, 2, -2, .7f, tint);
  const auto beyond = quad(0, .9f, -2, 2, -2, 2, black);
  remixapi_CameraTemporalEXT epoch{};
  epoch.sType = REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT;
  epoch.worldEpoch = 1;
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &epoch;
  for (int i = 0; i < 4; ++i)
    camera.view[i][i] = camera.projection[i][i] = 1;
  remixapi_LightInfoSphereEXT sphere{};
  sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
  sphere.position = {.5f, 0, .1f};
  sphere.radius = .08f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  light.hash = next++;
  light.radiance = {20, 10, 4};
  remixapi_LightHandle torch{};
  ok(api.CreateLight(&light, &torch));
  std::vector<remixapi_LightHandle> lights{torch};
  std::vector<remixapi_MeshHandle> occluders;
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable(
        (std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  auto draw = [&](remixapi_MeshHandle mesh) {
    remixapi_InstanceIdentityEXT identity{};
    identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
    identity.instanceId = reinterpret_cast<uintptr_t>(mesh);
    identity.rasterVisible = 1;
    identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
    remixapi_InstanceInfoBlendEXT blend{};
    blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
    blend.pNext = &identity;
    blend.alphaBlendEnabled = mesh == glass;
    remixapi_InstanceInfo instance{};
    instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    instance.pNext = &blend;
    instance.mesh = mesh;
    instance.doubleSided = 1;
    for (int i = 0; i < 3; ++i)
      instance.transform.matrix[i][i] = 1;
    ok(api.DrawInstance(&instance));
  };
  auto frame = [&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    for (auto emitter : lights)
      ok(api.DrawLightInstance(emitter));
    draw(receiver);
    for (auto mesh : occluders)
      draw(mesh);
    ok(api.Present(nullptr));
  };
  auto temporal = [&] {
    remixapi_DenoisingStatisticsEXT stats{};
    stats.structSize = sizeof(stats);
    ok(remixapi_GetDenoisingStatisticsEXT(&stats));
    return stats;
  };
  auto local = [&] {
    remixapi_LocalLightStatisticsEXT stats{};
    stats.structSize = sizeof(stats);
    ok(remixapi_GetLocalLightStatisticsEXT(&stats));
    return stats;
  };
  auto tracing = [&] {
    remixapi_TracingStatisticsEXT stats{};
    stats.structSize = sizeof(stats);
    ok(remixapi_GetTracingStatisticsEXT(&stats));
    return stats;
  };
  auto readback = [&](const std::string &name) {
    const auto file = (directory / (name + ".png")).string();
    ok(api.RequestPresentedScreenshot(file.c_str()));
    frame();
    frame();
    int width, height, channels;
    auto pixels = stbi_load(file.c_str(), &width, &height, &channels, 4);
    require(pixels, "Missing local-light screenshot");
    std::array<float, 3> average{};
    for (int y = height / 2 - 4; y < height / 2 + 4; ++y)
      for (int x = width / 2 - 4; x < width / 2 + 4; ++x)
        for (int c = 0; c < 3; ++c) {
          float v = pixels[(y * width + x) * 4 + c] / 255.f;
          average[c] += (v <= .04045f ? v / 12.92f
                                      : std::pow((v + .055f) / 1.055f, 2.4f)) /
                        64;
        }
    stbi_image_free(pixels);
    std::printf("%s linear RGB: %.5f %.5f %.5f\n", name.c_str(), average[0],
                average[1], average[2]);
    std::fflush(stdout);
    return average;
  };
  auto capture = [&](const std::string &name, unsigned frames) {
    ++epoch.historyResetEpoch;
    for (unsigned i = 0; i < frames; ++i)
      frame();
    return readback(name);
  };
  if (std::getenv("REMIX_TEST_FAULT")) {
    config("accumulate", "0");
    const auto value = capture("fallback", 48);
    const auto stats = local();
    require(value[0] > .1f && !stats.denoiserActive &&
                stats.unavailableReason[0],
            "Failed local resources did not retain raw torch lighting with a "
            "reason");
    require(temporal().activeDenoiser && temporal().activeTaa,
            "Local fallback disabled established SH/TAA");
    ok(api.Shutdown());
    return 0;
  }
  if (std::getenv("REMIX_TEST_LOCAL_PERF")) {
    // Tiny continuous camera motion requests real renders after histories warm.
    // Otherwise the stationary-frame cache would make timing samples
    // misleading.
    const char *samples = std::getenv("REMIX_TEST_LOCAL_SAMPLES");
    config("localSamples", samples ? samples : "2");
    for (bool filtered : {false, true}) {
      config("denoiser", filtered ? "reblur-sh" : "off");
      config("taa", filtered ? "1" : "0");
      config("sigma", filtered ? "1" : "0");
      for (unsigned count : {0u, 100u, 1000u, 5000u}) {
        for (auto emitter : lights)
          ok(api.DestroyLight(emitter));
        lights.clear();
        const unsigned side = unsigned(std::ceil(std::sqrt(count)));
        light.radiance = {20, 10, 4};
        for (unsigned i = 0; i < count; ++i) {
          sphere.position = {2.f * (float(i % side) - .5f * (side - 1)) + .5f,
                             2.f * (float(i / side) - .5f * (side - 1)), .1f};
          light.hash = next++;
          remixapi_LightHandle emitter{};
          ok(api.CreateLight(&light, &emitter));
          lights.push_back(emitter);
        }
        ++epoch.historyResetEpoch;
        std::vector<float> times, filters, confidence, indirect, taa;
        for (unsigned i = 0; i < 112; ++i) {
          camera.view[0][3] = i % 2 ? .00001f : 0;
          frame();
          if (i >= 64) {
            const auto t = tracing();
            const auto l = local();
            const auto d = temporal();
            require(t.gpuTimingsAvailable,
                    "GPU timing unavailable for torch performance probe");
            times.push_back(t.gpuTraceMilliseconds);
            filters.push_back(float(l.gpuDenoiseMilliseconds));
            confidence.push_back(d.gpuConfidenceMilliseconds);
            indirect.push_back(d.gpuDenoiseMilliseconds);
            taa.push_back(d.gpuTaaMilliseconds);
          }
        }
        auto median = [](std::vector<float> &values) {
          std::sort(values.begin(), values.end());
          return values[values.size() / 2];
        };
        const auto l = local();
        std::printf("PERF filtered=%u lights=%u samples=%u depth=%u "
                    "trace_ms=%.4f local_nrd_ms=%.4f confidence_ms=%.4f "
                    "indirect_nrd_ms=%.4f taa_ms=%.4f bvh_bytes=%llu\n",
                    unsigned(filtered), count, l.samples, l.treeDepth,
                    median(times), median(filters), median(confidence),
                    median(indirect), median(taa),
                    (unsigned long long)l.memoryBytes);
        std::fflush(stdout);
        readback(std::string(filtered ? "filtered-" : "raw-") +
                 std::to_string(count));
      }
    }
    ok(api.Shutdown());
    return 0;
  }
  remixapi_LocalLightStatisticsEXT invalid{};
  require(remixapi_GetLocalLightStatisticsEXT(nullptr) ==
                  REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS &&
              remixapi_GetLocalLightStatisticsEXT(&invalid) ==
                  REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
          "Malformed local diagnostics accepted");
  for (const char *primary : {"raster", "ray"})
    for (bool filtered : {false, true}) {
      config("primaryVisibility", primary);
      config("accumulate", filtered ? "0" : "1");
      config("localSamples", "2");
      const std::string prefix =
          std::string(primary) + (filtered ? "-nrd-" : "-raw-");
      const unsigned frames = filtered ? 48 : 32;
      light.radiance = {20, 10, 4};
      ok(api.UpdateLightDefinition(torch, &light));
      occluders.clear();
      auto open = capture(prefix + "open", frames);
      const double d2 = .5 * .5 + .7 * .7;
      const double expected = 20 * .08 * .08 / d2 * .7 / std::sqrt(d2);
      require(std::abs(open[0] - expected) < .018 &&
                  std::abs(open[1] - open[0] * .5) < .008 &&
                  std::abs(open[2] - open[0] * .2) < .008,
              "Analytic sphere energy or NRD color packing incorrect");
      require(!filtered || local().denoiserActive,
              "Local NRD did not activate");
      if (filtered) {
        config("localDenoiser", "0");
        const auto unfiltered = capture(prefix + "raw-local-sigma", 32);
        remixapi_ShadowStatisticsEXT shadows{};
        shadows.structSize = sizeof(shadows);
        ok(remixapi_GetShadowStatisticsEXT(&shadows));
        require(
            !local().denoiserActive && shadows.active &&
                std::abs(unfiltered[0] - expected) < .018,
            "Raw local radiance was lost or modulated by the sun's SIGMA mask");
        config("localDenoiser", "1");
      }
      occluders = {blocker};
      auto closed = capture(prefix + "closed", frames);
      require(closed[0] < .005f, "Opaque wall leaked torch light");
      occluders = {holes};
      auto cut = capture(prefix + "cutout", frames);
      require(std::abs(cut[0] - open[0]) < .015,
              "Alpha cutout blocked torch light");
      occluders = {glass};
      auto tinted = capture(prefix + "tinted", frames);
      require(tinted[1] > .015 && tinted[1] < open[1] * .8 &&
                  tinted[0] < tinted[1] * .3,
              "Tinted transparent torch shadows lost color/attenuation");
      occluders = {beyond};
      auto behind = capture(prefix + "beyond-light", frames);
      require(std::abs(behind[0] - open[0]) < .015,
              "Geometry beyond the emitter blocked its shadow ray");
      occluders.clear();
      config("localLights", "0");
      auto disabled = capture(prefix + "disabled", frames);
      require(disabled[0] < .005, "Disabled local lights still contributed");
      config("localLights", "1");
      frame();
      frame();
      const auto resets = temporal().historyResets, builds = local().builds;
      auto previous = tracing();
      for (int i = 0; i < 12; ++i) {
        light.radiance =
            i % 2 ? remixapi_Float3D{20, 10, 4} : remixapi_Float3D{};
        ok(api.UpdateLightDefinition(torch, &light));
        frame();
        const auto now = tracing();
        require(now.sceneGeneration > previous.sceneGeneration,
                "Torch toggle reused a cached frame");
        if (filtered)
          require(temporal().historyResets == resets,
                  "Redstone toggle reset global temporal history");
        else
          require(now.accumulationCount == 1,
                  "Torch change retained progressive accumulation");
        require(local().builds == builds,
                "Power toggle rebuilt LightBVH topology");
        previous = now;
      }
      light.radiance = {};
      ok(api.UpdateLightDefinition(torch, &light));
      for (int i = 0; i < 8; ++i)
        frame();
      require(readback(prefix + "off")[0] < .005,
              "Unlit redstone retained direct light");
      light.radiance = {20, 10, 4};
      ok(api.UpdateLightDefinition(torch, &light));
    }
  // The receiver is in direct shadow, while white side walls can receive torch
  // light around a short partition and return it through diffuse bounces.
  const auto partition = quad(0, .25f, -.3f, .3f, -2, .7f, black);
  const auto sideA = quad(1, 1, -1, .8f, -1, 1, white);
  const auto sideB = quad(1, -1, -1, .8f, -1, 1, white);
  const auto sideC = quad(0, -1, -1, 1, -1, .8f, white);
  occluders = {partition, sideA, sideB, sideC};
  config("primaryVisibility", "raster");
  config("accumulate", "1");
  require(capture("bounce-disabled", 16)[0] < .005,
          "Zero-bounce partition admitted indirect local light");
  config("bounces", "1");
  config("samples", "8");
  const auto bounced = capture("bounce-reference", 64);
  require(bounced[0] > .005,
          "Diffuse continuation did not receive torch lighting");
  config("accumulate", "0");
  config("samples", "2");
  config("sharc", "1");
  config("sharcCapacityLog2", "18");
  config("sharcDownscale", "2");
  const auto cached = capture("bounce-sharc-nrd", 128);
  remixapi_SharcStatisticsEXT cache{};
  cache.structSize = sizeof(cache);
  ok(remixapi_GetSharcStatisticsEXT(&cache));
  require(cache.active && cache.queryHits > 0,
          "Torch scene did not exercise SHARC reuse");
  require(cached[0] > .003 && std::abs(cached[0] - bounced[0]) <
                                  std::max(.012f, bounced[0] * .4f),
          "SHARC torch radiance disagrees with bounced reference");
  const auto resets = temporal().historyResets;
  light.radiance = {};
  ok(api.UpdateLightDefinition(torch, &light));
  for (int i = 0; i < 96; ++i)
    frame();
  require(
      readback("bounce-off")[0] < .005 && temporal().historyResets == resets,
      "Torch extinction left cached light or reset global temporal history");
  occluders.clear();
  config("bounces", "0");
  config("samples", "1");
  config("sharc", "0");
  // Compare many-light estimates against the same sphere integral. Duplicate
  // analytic sources have fixed total power: selection count cannot change
  // energy.
  config("primaryVisibility", "raster");
  config("accumulate", "1");
  for (unsigned count : {100u, 1000u, 5000u}) {
    for (auto emitter : lights)
      ok(api.DestroyLight(emitter));
    lights.clear();
    light.radiance = {20.f / count, 10.f / count, 4.f / count};
    for (unsigned i = 0; i < count; ++i) {
      light.hash = next++;
      remixapi_LightHandle handle{};
      ok(api.CreateLight(&light, &handle));
      lights.push_back(handle);
    }
    const auto result = capture("many-" + std::to_string(count), 16);
    require(std::abs(result[0] - .1407f) < .012,
            "Many-light selection changed total energy");
    const auto stats = local();
    require(stats.lightCount == count && stats.nodeCount == count * 2 - 1 &&
                stats.treeDepth <= 14,
            "Large hierarchy count/depth is invalid");
    std::printf(
        "lights=%u nodes=%u depth=%u trace_ms=%.4f cpu_update_ms=%.4f\n", count,
        stats.nodeCount, stats.treeDepth, tracing().gpuTraceMilliseconds,
        stats.cpuUpdateMilliseconds);
  }
  for (auto emitter : lights)
    ok(api.DestroyLight(emitter));
  lights.clear();
  require(capture("unloaded", 4)[0] < .005,
          "Unloaded lights survived renderer submission");
  ok(api.Shutdown());
  std::puts("Analytic local lights, visibility, radiance NRD, temporal updates "
            "and 5000-light hierarchy passed");
}
