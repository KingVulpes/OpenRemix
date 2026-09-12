#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <string>
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
void ok(remixapi_ErrorCode code) {
  require(code == REMIXAPI_ERROR_CODE_SUCCESS, "Torch stability API failure");
}
using Image = std::vector<std::array<float, 3>>;
} // namespace
int main(int argc, char **argv) {
  require(argc == 2, "Supply capture directory");
  auto directory = std::filesystem::absolute(argv[1]);
  std::filesystem::create_directories(directory);
  remixapi_Interface api{};
  remixapi_InitializeLibraryInfo init{};
  init.sType = REMIXAPI_STRUCT_TYPE_INITIALIZE_LIBRARY_INFO;
  init.version = REMIXAPI_VERSION_MAKE(0, 1003, 0);
  ok(remixapi_InitializeLibrary(&init, &api));
  remixapi_StartupInfo startup{};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = std::getenv("REMIX_TEST_MIXED_PERF") ? 1920 : 160;
  startup.windowHeight = std::getenv("REMIX_TEST_MIXED_PERF") ? 1080 : 128;
#endif
  ok(api.Startup(&startup));
  if (std::getenv("REMIX_TEST_MIXED_PERF")) {
    remixapi_WindowMetrics metrics{};
    ok(remixapi_GetWindowMetrics(&metrics));
    require(metrics.drawableWidth == 1920 && metrics.drawableHeight == 1080,
            "Mixed-light timing requires a 1920x1080 drawable");
  }
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  if (!(caps.supportedBackends & 2)) {
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable(
        (std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  config("sharc", "0");
  config("sky", "0");
  config("sigma", "0");
  config("localRange", "16");
  config("localIntensity", "1");
  config("adaptiveLocalSampling", std::getenv("REMIX_TEST_ADAPTIVE_LOCAL") ? "1" : "0");
  if (const char *split = std::getenv("REMIX_TEST_SPLIT_LOCAL"))
    config("separateLocalLighting", split);
  const bool mixed = std::getenv("REMIX_TEST_MIXED_LIGHTS");
  if (mixed) {
    config("prism", "0");
    config("gamma22", "0");
  }
  uint64_t next = 1;
  auto material = [&](remixapi_Float3D color, float alpha, float emission = 0.f) {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = color;
    opaque.opacityConstant = alpha;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    info.pNext = &opaque;
    info.hash = next++;
    info.emissiveIntensity = emission;
    info.emissiveColorConstant = {1, .5f, .2f};
    remixapi_MaterialHandle result{};
    ok(api.CreateMaterial(&info, &result));
    return result;
  };
  auto white = material({.8f, .8f, .8f}, 1), black = material({0, 0, 0}, 1);
  auto blue = material({.3f, .65f, .9f}, .6f);
  auto quad = [&](int axis, float plane, float a0, float a1, float b0, float b1,
                  remixapi_MaterialHandle material) {
    remixapi_HardcodedVertex v[4]{};
    uint32_t indices[] = {0, 1, 2, 0, 2, 3};
    for (int i = 0; i < 4; ++i) {
      v[i].position[axis] = plane;
      v[i].position[(axis + 1) % 3] = i == 0 || i == 3 ? a0 : a1;
      v[i].position[(axis + 2) % 3] = i < 2 ? b0 : b1;
      v[i].color = 0xffffffff;
    }
    remixapi_MeshInfoSurfaceTriangles surface{};
    surface.vertices_values = v;
    surface.vertices_count = 4;
    surface.indices_values = indices;
    surface.indices_count = 6;
    surface.material = material;
    remixapi_MeshInfo mesh{};
    mesh.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    mesh.hash = next++;
    mesh.surfaces_values = &surface;
    mesh.surfaces_count = 1;
    remixapi_MeshHandle result{};
    ok(api.CreateMesh(&mesh, &result));
    return result;
  };
  auto receiver = quad(2, .8f, -2, 2, -2, 2, white),
       background = quad(2, .8f, -2, 2, -2, 2, black);
  auto water = quad(2, .3f, -2, 2, -2, 2, blue);
  auto partition = quad(0, .25f, -.35f, .35f, -2, .7f, black);
  auto sideA = quad(1, 1, -1, .8f, -1, 1, white),
       sideB = quad(1, -1, -1, .8f, -1, 1, white);
  auto sideC = quad(0, -1, -1, 1, -1, .8f, white);
  std::vector<remixapi_MeshHandle> emitters;
  if (mixed) {
    auto glowing = material({0, 0, 0}, 1, .3f);
    if (std::getenv("REMIX_TEST_NEAR_EMITTERS")) {
      // Broad emitter just outside the central receiver ROI; its solid angle
      // changes across the wall and during the same translated camera path.
      emitters.push_back(quad(1, -.6f, 0, .7f, -.8f, .8f, glowing));
    }
    const bool many = std::getenv("REMIX_TEST_MIXED_MANY") || std::getenv("REMIX_TEST_EMISSIVE_CLUSTERS");
    for (int i = 0; i < (std::getenv("REMIX_TEST_NEAR_EMITTERS") ? 0 : many ? 512 : 4); ++i) {
      float x = -.8f + (many ? (i % 32) * .05f : i * .4f);
      float y = many ? -.4f + (i / 32) * .05f : -.3f;
      emitters.push_back(quad(2, -.25f, x, x + (many ? .04f : .2f),
                              y, y + (many ? .04f : .6f), glowing));
    }
  }
  std::vector<remixapi_MeshHandle> meshes;
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
  sphere.radius = .04f;
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &sphere;
  std::array<remixapi_LightHandle, 4> lights{};
  for (auto &handle : lights) {
    light.hash = next++;
    ok(api.CreateLight(&light, &handle));
  }
  auto frame = [&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    for (auto handle : lights)
      ok(api.DrawLightInstance(handle));
    for (auto mesh : meshes) {
      remixapi_InstanceIdentityEXT identity{};
      identity.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT;
      identity.instanceId = reinterpret_cast<uintptr_t>(mesh);
      identity.rasterVisible = 1;
      identity.classification = REMIXAPI_INSTANCE_STATIC_EXT;
      remixapi_InstanceInfoBlendEXT blend{};
      blend.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT;
      blend.pNext = &identity;
      blend.alphaBlendEnabled = mesh == water;
      remixapi_InstanceInfo instance{};
      instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
      instance.pNext = &blend;
      instance.mesh = mesh;
      instance.doubleSided = 1;
      for (int i = 0; i < 3; ++i)
        instance.transform.matrix[i][i] = 1;
      ok(api.DrawInstance(&instance));
    }
    ok(api.Present(nullptr));
  };
  auto capture = [&](const std::string &name) {
    auto file = (directory / (name + ".png")).string();
    ok(api.RequestPresentedScreenshot(file.c_str()));
    frame();
    frame();
    int w, h, n;
    auto data = stbi_load(file.c_str(), &w, &h, &n, 4);
    require(data, "Torch capture unavailable");
    Image result;
    for (int y = h / 4; y < h * 3 / 4; ++y)
      for (int x = w / 4; x < w * 3 / 4; ++x) {
        std::array<float, 3> pixel{};
        for (int c = 0; c < 3; ++c) {
          float v = data[(y * w + x) * 4 + c] / 255.f;
          pixel[c] =
              v <= .04045f ? v / 12.92f : std::pow((v + .055f) / 1.055f, 2.4f);
        }
        result.push_back(pixel);
      }
    stbi_image_free(data);
    return result;
  };
  auto stats = [&] {
    remixapi_DenoisingStatisticsEXT s{};
    s.structSize = sizeof(s);
    ok(remixapi_GetDenoisingStatisticsEXT(&s));
    return s;
  };
  auto error = [&](const Image &image, const Image &reference) {
    double mse = 0, mean = 0, truth = 0;
    std::vector<double> excess;
    for (size_t i = 0; i < image.size(); ++i) {
      auto luminance = [](const auto &rgb) {
        return .2126 * rgb[0] + .7152 * rgb[1] + .0722 * rgb[2];
      };
      double a = luminance(image[i]), b = luminance(reference[i]);
      mse += (a - b) * (a - b);
      mean += a;
      truth += b;
      excess.push_back(std::max(0., a - b));
    }
    std::sort(excess.begin(), excess.end());
    return std::array<double, 4>{std::sqrt(mse / image.size()),
                                 mean / image.size(), truth / image.size(),
                                 excess[size_t(.999 * (excess.size() - 1))]};
  };
  auto csv = std::fopen((directory / "metrics.csv").c_str(), "w");
  require(csv, "Cannot open metrics");
  std::fputs(
      "primary,signal,mode,pose,rmse,mean,reference,p999_excess,gpu_trace_ms\n",
      csv);
  const char *onlyPrimary = std::getenv("REMIX_TEST_PRIMARY");
  for (const char *primary : {"raster", "ray"}) {
    if (onlyPrimary && std::string(primary) != onlyPrimary)
      continue;
    config("primaryVisibility", primary);
    for (int signal = 0; signal < 3; ++signal) {
      config("torchLights", "1");
      meshes = signal == 2 ? std::vector<remixapi_MeshHandle>{background, water}
                           : std::vector<remixapi_MeshHandle>{receiver};
      if (signal == 1)
        meshes.insert(meshes.end(), {partition, sideA, sideB, sideC});
      meshes.insert(meshes.end(), emitters.begin(), emitters.end());
      for (unsigned i = 0; i < lights.size(); ++i) {
        sphere.position = {.5f + float(i & 1) * .15f,
                           (float(i >> 1) - .5f) * .25f,
                           signal == 2 ? -.4f : .1f};
        light.radiance = i == 0   ? remixapi_Float3D{20, 10, 4}
                         : i == 3 ? remixapi_Float3D{6, .6f, .2f}
                                  : remixapi_Float3D{4, 2, .8f};
        ok(api.UpdateLightDefinition(lights[i], &light));
      }
      const std::string prefix =
          std::string(primary) + "-" + std::to_string(signal);
      config("bounces", signal == 1 ? "1" : "0");
      config("localGuiding", mixed ? "0" : "1");
      config("emissiveCandidates", "1");
      if (mixed) {
        auto count = [&] {
          frame();
          remixapi_LocalLightStatisticsEXT s{};
          s.structSize = sizeof(s);
          ok(remixapi_GetLocalLightStatisticsEXT(&s));
          return s.activeLights;
        };
        require(count() == 4 + 2 * emitters.size(), "Mixed fixture lost emitters");
        config("torchLights", "0");
        require(count() == 2 * emitters.size(), "Torch toggle removed emissive lights");
        config("emissiveLights", "0");
        require(count() == 0, "Casting toggles retained lights");
        config("torchLights", "1");
        require(count() == 4, "Emissive toggle removed torches");
        config("emissiveLights", "1");
      }
      // Exercise a non-root area-light guide subtree without a brighter
      // analytic torch winning every cached preference in this tiny fixture.
      if (std::getenv("REMIX_TEST_EMISSIVE_CLUSTERS"))
        config("torchLights", "0");
      if (std::getenv("REMIX_TEST_MIXED_PERF")) {
        config("accumulate", "0");
        config("localSamples", "1");
        config("samples", "1");
        config("denoiser", "reblur-sh");
        config("taa", "1");
        config("localGuiding", "1");
        for (const char *candidates : {"1", "4"}) {
          config("emissiveCandidates", candidates);
          ++epoch.historyResetEpoch;
          for (int f = 0; f < 64; ++f) {
            camera.view[3][0] = float(f % 16) * .006f;
            frame();
            if (f < 32) continue;
            remixapi_TracingStatisticsEXT tracing{};
            tracing.structSize = sizeof(tracing);
            ok(remixapi_GetTracingStatisticsEXT(&tracing));
            std::printf("PERF %s signal=%d emitters=%zu candidates=%s frame=%d trace_ms=%.6f\n",
                        primary, signal, 4 + 2 * emitters.size(), candidates, f,
                        tracing.gpuTraceMilliseconds);
          }
        }
        continue;
      }
      std::vector<Image> reference;
      config("emissiveSolidAngleSampling", "0");
      // Compare split estimators to the established combined-light integral,
      // not to a reference produced by the same family-selection code.
      if (std::getenv("REMIX_TEST_SPLIT_LOCAL"))
        config("separateLocalLighting", "0");
      // References use exactly the camera poses later traversed by the moving
      // run.
      for (int pose = 0; pose < 16; ++pose) {
        camera.view[3][0] = float(pose) * .006f;
        config("accumulate", "1");
        config("localSamples", "16");
        config("samples", "16");
        ++epoch.historyResetEpoch;
        for (int f = 0; f < 24; ++f)
          frame();
        reference.push_back(
            capture(prefix + "-reference-" + std::to_string(pose)));
        remixapi_TracingStatisticsEXT tracing{};
        tracing.structSize = sizeof(tracing);
        ok(remixapi_GetTracingStatisticsEXT(&tracing));
        require(tracing.accumulationCount >= 24,
                "Torch reference failed to accumulate");
      }
      config("localSamples", "1");
      config("samples", "1");
      if (const char *split = std::getenv("REMIX_TEST_SPLIT_LOCAL"))
        config("separateLocalLighting", split);
      config("emissiveSolidAngleSampling", std::getenv("REMIX_TEST_SOLID_ANGLE") ? std::getenv("REMIX_TEST_SOLID_ANGLE") : "0");
      double baselineError = 0;
      for (int run = 0; run < (mixed ? 6 : 3); ++run) {
        int mode = run % 3;
        config("emissiveCandidates", run < 3 ? "1" : "4");
        config("localGuiding", "1");
        config("accumulate", "0");
        config("denoiser", mode == 0 ? "off" : "reblur-sh");
        config("taa", mode == 0 ? "0" : "1");
        camera.view[3][0] = 0;
        ++epoch.historyResetEpoch;
        for (int f = 0; f < 64; ++f)
          frame();
        const auto resets = stats().historyResets;
        double normalizedError = 0, observed = 0, expected = 0;
        for (int sample = 0; sample < 16; ++sample) {
          int pose = mode == 2 ? sample : 0;
          camera.view[3][0] = float(pose) * .006f + (sample & 1) * .000001f;
          auto image = capture(prefix + "-" + std::to_string(run) + "-" +
                               std::to_string(sample));
          auto e = error(image, reference[pose]);
          normalizedError += e[0] / std::max(e[2], .000001);
          observed += e[1];
          expected += e[2];
          remixapi_TracingStatisticsEXT tracing{};
          tracing.structSize = sizeof(tracing);
          ok(remixapi_GetTracingStatisticsEXT(&tracing));
          std::fprintf(csv, "%s,%d,%d,%d,%.9f,%.9f,%.9f,%.9f,%.6f\n", primary,
                       signal, run, pose, e[0], e[1], e[2], e[3],
                       tracing.gpuTraceMilliseconds);
          require(e[1] < std::max(.01, e[2] * 2.5),
                  "Torch estimator produced excessive mean energy");
        }
        // At the visible opaque receiver the four-light candidate estimator
        // must converge within one ray/pixel, before any denoising can hide it.
        if (mode == 0 && signal == 0 && !mixed && !std::getenv("REMIX_TEST_BASELINE"))
          require(normalizedError / 16 < .04,
                  "Four-light selection produced excessive one-sample error");
        // With a shared family budget, category selection introduces variance
        // independent of the candidate count. Compare quality in the saved
        // scene; the split regressions enforce the combined reference energy.
        if (mixed && mode == 0 && signal == 0 && !std::getenv("REMIX_TEST_SPLIT_LOCAL")) {
          if (run == 0)
            baselineError = normalizedError;
          else
            require(normalizedError < baselineError * .85,
                    "Mixed-light RIS did not reduce raw one-ray error");
        }
        require(std::abs(observed - expected) < std::max(.004, expected * .12),
                "Torch filtering changed average scene energy");
        if (std::getenv("REMIX_TEST_EMISSIVE_CLUSTERS") && run == 3 && signal == 1) {
          remixapi_LocalLightGuidingStatisticsEXT guidance{};
          guidance.structSize = sizeof(guidance);
          ok(remixapi_GetLocalLightGuidingStatisticsEXT(&guidance));
          require(guidance.active && guidance.sampledGuided > 0,
                  "Area-light cluster PDF test did not exercise guided proposals");
        }
        require(mode == 0 ||
                    (stats().activeTaa && stats().historyResets == resets),
                "Camera motion reset torch temporal history");
        std::fflush(csv);
      }
    }
  }
  std::fclose(csv);
  ok(api.Shutdown());
  std::puts(mixed ? "Mixed torch/emissive sampling checks passed"
                  : "Four-torch direct, bounced and water motion captures passed");
}
