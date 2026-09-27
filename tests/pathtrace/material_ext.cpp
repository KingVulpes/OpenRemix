// GPU check of the fork material extensions (remix_fo4_ext.h) against the
// dxvk-remix fork's formulas: each spec/gloss or albedo-tint material must
// render like a plain material whose constants hold the values the fork's
// decode produces for the same texels. Covers the path tracer (raster and ray
// primary visibility), the raster backend and BC textures kept compressed.
// Needs a display and a ray-tracing Vulkan device: runs when
// REMIX_TEST_MATERIAL_GPU is set (meson sets it with -Dgpu_tests=true).
#include "runtime/prepared.h"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <remix/remix_native_ext.h>
#include <string>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include <image/stb_image.h>

namespace {
  void require(bool v, const char *why) {
    if (!v) {
      std::fprintf(stderr, "%s\n", why);
      std::exit(1);
    }
  }
  void ok(remixapi_ErrorCode code) {
    if (code) {
      std::fprintf(stderr, "Material extension API error %x\n", unsigned(code));
      std::exit(1);
    }
  }
  double srgb(double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); }
  double gamma(double v) { return std::pow(v, 2.2); } // the fork's gammaToLinear
  uint32_t q(float v, float maxCode) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * maxCode + 0.5f); }

  struct Case {
    remixapi_Path albedo = nullptr, roughness = nullptr;
    remixapi_Float3D albedoConstant = {1, 1, 1};
    float roughnessConstant = .6f, metallic = 0;
    bool specGloss = false;
    uint32_t encoding = 3;
    float strength = 0, low = .35f, high = .65f, lumMax = 1;
    remixapi_Float3D specularTint = {0, 0, 0};
    bool tinted = false;
    remixapi_Float3D albedoTint = {1, 1, 1};
  };

  // The fork's spec/gloss metalness (opaque_surface_material_interaction.slangh)
  // on a linear albedo, with its parameter quantization.
  double forkMetal(const Case &c, double spec, std::array<double, 3> &albedo) {
    const double low = q(c.low, 15) / 15., high = q(c.high, 15) / 15., strength = q(c.strength, 15) / 15.;
    const uint32_t lumCode = q(c.lumMax, 15);
    const std::array<double, 3> tint = {q(c.specularTint.x, 31) / 31., q(c.specularTint.y, 63) / 63.,
                                        q(c.specularTint.z, 31) / 31.};
    const bool hasTint = tint[0] > 0 || tint[1] > 0 || tint[2] > 0;
    const double level = hasTint ? spec * std::max({tint[0], tint[1], tint[2]}) : spec;
    double mask = std::clamp((level - low) / std::max(high - low, 1. / 255), 0., 1.);
    if (lumCode < 15) {
      const double lm = gamma(lumCode / 15.), lum = .2126 * albedo[0] + .7152 * albedo[1] + .0722 * albedo[2];
      const double t = std::clamp((lum - .25 * lm) / (.75 * lm), 0., 1.);
      mask *= 1 - t * t * (3 - 2 * t);
    }
    const double metal = strength * mask;
    if (hasTint)
      for (int i = 0; i < 3; ++i)
        albedo[i] += (gamma(std::clamp(spec * tint[i], 0., 1.)) - albedo[i]) * metal;
    return metal;
  }
} // namespace

int main(int argc, char **argv) {
  if (!std::getenv("REMIX_TEST_MATERIAL_GPU")) {
    std::puts("Material extension GPU check skipped (set REMIX_TEST_MATERIAL_GPU=1)");
    return 77;
  }
  require(argc == 2, "Supply a screenshot directory");
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
  startup.windowWidth = startup.windowHeight = 64;
#endif
  ok(api.Startup(&startup));
  ok(api.SetConfigVariable("rtx.native.pathtrace.autoExposure", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.bloom", "0"));
  ok(api.SetConfigVariable("rtx.native.pathtrace.upscaler", "off"));
  remixapi_BackendCapabilitiesEXT caps{};
  caps.structSize = sizeof(caps);
  ok(remixapi_GetBackendCapabilitiesEXT(&caps));
  if (!(caps.supportedBackends & 2)) {
    std::printf("Path tracing unavailable: %s\n", caps.unavailableReason);
    ok(api.Shutdown());
    return 77;
  }
  ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
  ok(api.SetConfigVariable("rtx.native.vsync", "0"));
  auto config = [&](const char *key, const char *value) {
    ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
  };
  for (const char *key : {"taa", "sharc", "sigma", "prism", "gamma22"})
    config(key, "0");
  config("denoiser", "off");
  config("samples", "16");
  config("bounces", "1");
  config("accumulate", "1");

  // Textures. The BC ones are kept compressed on the GPU.
  auto texture = [&](uint64_t id, remixapi_Format format, uint32_t size, const void *data, uint64_t bytes) {
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = id;
    info.width = info.height = size;
    info.depth = info.mipLevels = 1;
    info.format = format;
    info.data = data;
    info.dataSize = bytes;
    remixapi_TextureHandle handle;
    ok(api.CreateTexture(&info, &handle));
  };
  auto rgba = [&](uint64_t id, remixapi_Format format, std::array<uint8_t, 4> pixel) {
    texture(id, format, 1, pixel.data(), 4);
  };
  rgba(101, REMIXAPI_FORMAT_R8G8B8A8_UNORM, {204, 102, 0, 255}); // spec .8, gloss .4
  rgba(102, REMIXAPI_FORMAT_R8G8B8A8_UNORM, {0, 255, 0, 255});   // gloss 1: the floor wins
  rgba(103, REMIXAPI_FORMAT_R8G8B8A8_SRGB, {128, 200, 64, 255});
  rgba(104, REMIXAPI_FORMAT_R8G8B8A8_UNORM, {128, 200, 64, 255});
  rgba(107, REMIXAPI_FORMAT_R8G8B8A8_UNORM, {102, 255, 0, 255}); // smoothness .4 in red
  ok(api.SetConfigVariable("rtx.native.textures.keepCompressed", "1"));
  // BC5: two BC4 blocks with both endpoints equal and all indices 0.
  const uint8_t bc5[16] = {204, 204, 0, 0, 0, 0, 0, 0, 102, 102, 0, 0, 0, 0, 0, 0};
  texture(105, REMIXAPI_FORMAT_BC5_UNORM, 4, bc5, sizeof(bc5));
  // BC1: color0 == color1 (565: r 16, g 50, b 8), all indices 0.
  const uint16_t c565 = uint16_t(16 << 11 | 50 << 5 | 8);
  const uint8_t bc1[8] = {uint8_t(c565), uint8_t(c565 >> 8), uint8_t(c565), uint8_t(c565 >> 8), 0, 0, 0, 0};
  texture(106, REMIXAPI_FORMAT_BC1_RGB_SRGB, 4, bc1, sizeof(bc1));
  const std::array<double, 3> bc1Texel = {(16 << 3 | 16 >> 2) / 255., (50 << 2 | 50 >> 4) / 255.,
                                          (8 << 3 | 8 >> 2) / 255.};
  const auto sg = std::filesystem::path("0x65"), gloss1 = std::filesystem::path("0x66"),
             albedoSrgb = std::filesystem::path("0x67"), albedoUnorm = std::filesystem::path("0x68"),
             sgBc5 = std::filesystem::path("0x69"), albedoBc1 = std::filesystem::path("0x6a"),
             smoothRed = std::filesystem::path("0x6b");

  remixapi_CameraEnvironmentEXT environment{};
  environment.sType = REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT;
  environment.enabled = 1;
  environment.radiance = {.15f, .25f, .35f};
  remixapi_CameraInfo camera{};
  camera.sType = REMIXAPI_STRUCT_TYPE_CAMERA_INFO;
  camera.pNext = &environment;
  for (int i = 0; i < 4; ++i)
    camera.view[i][i] = camera.projection[i][i] = 1;
  remixapi_LightInfoDistantEXT distant{};
  distant.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT;
  distant.direction = {.6f, 0, .8f};
  remixapi_LightInfo light{};
  light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
  light.pNext = &distant;
  light.hash = 1;
  light.radiance = {1, 1, 1};
  remixapi_LightHandle sun;
  ok(api.CreateLight(&light, &sun));
  // Head-on light for the roughness cases: every pixel sees the mirror
  // direction, so the specular peak (GGX D) shows the roughness.
  distant.direction = {0, 0, 1};
  light.hash = 2;
  light.radiance = {.5f, .5f, .5f};
  remixapi_LightHandle head;
  ok(api.CreateLight(&light, &head));
  bool headOn = false;

  // One quad; each capture recreates its material under the same hash.
  remixapi_MaterialInfoOpaqueEXT opaque{};
  remixapi_MaterialInfoPbrEXT pbr{};
  remixapi_MaterialInfoOpaqueSpecGlossEXT specGloss{};
  remixapi_MaterialInfoOpaqueAlbedoTintEXT tint{};
  remixapi_MaterialInfo material{};
  material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
  material.hash = 20;
  auto describe = [&](const Case &c) {
    opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = c.albedoConstant;
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    opaque.roughnessConstant = c.roughnessConstant;
    opaque.metallicConstant = c.metallic;
    opaque.roughnessTexture = c.roughness;
    pbr = {};
    pbr.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT;
    pbr.enabled = 1;
    opaque.pNext = &pbr;
    void *next = nullptr; // opaque -> pbr -> [spec/gloss] -> [tint]
    if (c.tinted) {
      tint = {};
      tint.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_ALBEDO_TINT_EXT;
      tint.albedoTint = c.albedoTint;
      next = &tint;
    }
    if (c.specGloss) {
      specGloss = {};
      specGloss.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SPEC_GLOSS_EXT;
      specGloss.pNext = next;
      specGloss.roughnessEncoding = c.encoding;
      specGloss.metalStrength = c.strength;
      specGloss.specularMetalLow = c.low;
      specGloss.specularMetalHigh = c.high;
      specGloss.metalAlbedoLuminanceMax = c.lumMax;
      specGloss.specularTint = c.specularTint;
      next = &specGloss;
    }
    pbr.pNext = next;
    material.pNext = &opaque;
    material.albedoTexture = c.albedo;
  };
  describe({});
  remixapi_MaterialHandle mat;
  ok(api.CreateMaterial(&material, &mat));
  remixapi_HardcodedVertex vertices[4]{};
  for (unsigned i = 0; i < 4; ++i) {
    vertices[i].position[0] = (i == 0 || i == 3) ? -2 : 2;
    vertices[i].position[1] = i < 2 ? -2 : 2;
    vertices[i].position[2] = .5f;
    vertices[i].texcoord[0] = (i == 0 || i == 3) ? 0 : 1;
    vertices[i].texcoord[1] = i < 2 ? 0 : 1;
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
  mesh.hash = 30;
  mesh.surfaces_values = &surface;
  mesh.surfaces_count = 1;
  remixapi_MeshHandle geometry;
  ok(api.CreateMesh(&mesh, &geometry));
  remixapi_InstanceInfo instance{};
  instance.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
  instance.mesh = geometry;
  instance.doubleSided = 1;
  for (int i = 0; i < 3; ++i)
    instance.transform.matrix[i][i] = 1;
  auto frame = remix::test::preparedFrame([&] {
    ok(api.PumpEvents());
    ok(api.SetupCamera(&camera));
    ok(api.DrawLightInstance(headOn ? head : sun));
    ok(api.DrawInstance(&instance));
    ok(api.Present(nullptr));
  });
  auto capture = [&](const std::string &name, const Case &c) {
    describe(c);
    ok(api.CreateMaterial(&material, &mat));
    for (int i = 0; i < 16; ++i)
      frame();
    const auto file = (directory / (name + ".png")).string();
    ok(remix::test::requestScreenshot(api, file.c_str(), frame));
    frame();
    frame();
    int w, h, channels;
    auto *bytes = stbi_load(file.c_str(), &w, &h, &channels, 4);
    require(bytes, "Missing screenshot");
    std::array<double, 3> result{};
    int count = 0;
    for (int y = h / 4; y < 3 * h / 4; ++y)
      for (int x = w / 4; x < 3 * w / 4; ++x) {
        ++count;
        for (int channel = 0; channel < 3; ++channel)
          result[channel] += srgb(bytes[(y * w + x) * 4 + channel] / 255.);
      }
    stbi_image_free(bytes);
    for (auto &v : result)
      v /= count;
    std::printf("%s %.5f %.5f %.5f\n", name.c_str(), result[0], result[1], result[2]);
    std::fflush(stdout);
    return result;
  };
  auto same = [&](const std::string &name, const Case &extension, const Case &reference, const char *what) {
    const auto a = capture(name, extension), b = capture(name + "-reference", reference);
    for (int c = 0; c < 3; ++c)
      if (std::abs(a[c] - b[c]) >= .012) {
        std::fprintf(stderr, "%s: %s (channel %d: %.5f vs %.5f)\n", name.c_str(), what, c, a[c], b[c]);
        std::exit(1);
      }
  };
  auto constant = [](std::array<double, 3> albedo, float roughness, float metallic) {
    Case c;
    c.albedoConstant = {float(albedo[0]), float(albedo[1]), float(albedo[2])};
    c.roughnessConstant = roughness;
    c.metallic = metallic;
    return c;
  };

  auto pathTracer = [&](const std::string &mode) {
    // Roughness from gloss in green / smoothness in red, and the floor. A
    // mostly metallic surface, so the reflected sky shows the roughness.
    const std::array<double, 3> base = {.6, .5, .4};
    const float shiny = .9f;
    headOn = true;
    const auto expected = constant(base, .6f, shiny);
    {
      // The fixture must tell 1 - G (.6) from G (.4) and from 1 - R (.2).
      const auto a = capture("rough-.6-" + mode, expected), b = capture("rough-.4-" + mode, constant(base, .4f, shiny)),
                 c = capture("rough-.2-" + mode, constant(base, .2f, shiny));
      double ab = 0, ac = 0;
      for (int i = 0; i < 3; ++i) {
        ab = std::max(ab, std::abs(a[i] - b[i]));
        ac = std::max(ac, std::abs(a[i] - c[i]));
      }
      require(ab > .024 && ac > .024, "Roughness fixture is not sensitive to roughness");
    }
    Case green = constant(base, .1f, shiny);
    green.roughness = sg.c_str();
    green.specGloss = true;
    same("green-" + mode, green, expected, "Smoothness in green differs from 1 - G");
    Case red = green;
    red.roughness = smoothRed.c_str();
    red.encoding = 2;
    same("red-" + mode, red, expected, "Smoothness in red differs from 1 - R");
    Case floor = green;
    floor.roughness = gloss1.c_str();
    floor.roughnessConstant = .6f;
    same("floor-" + mode, floor, expected, "Roughness floor not applied");
    Case compressed = green;
    compressed.roughness = sgBc5.c_str();
    same("bc5-" + mode, compressed, expected, "Compressed spec/gloss map decoded differently");
    headOn = false;

    // Metalness from the specular level, with the albedo gate and the tint.
    Case metal = constant({.2, .15, .1}, .1f, 0);
    metal.roughness = sg.c_str();
    metal.specGloss = true;
    metal.strength = 1;
    metal.low = 0;
    metal.high = 1;
    metal.lumMax = .6f;
    metal.specularTint = {1, .5f, .25f};
    std::array<double, 3> albedo = {.2, .15, .1};
    const double metallic = forkMetal(metal, 204 / 255., albedo);
    require(metallic > .3 && metallic < .8, "Metal fixture outside the attenuation ramp");
    same("metal-" + mode, metal, constant(albedo, .6f, float(metallic)), "Specular metalness differs from the fork's");
    metal.roughness = sgBc5.c_str();
    same("metal-bc5-" + mode, metal, constant(albedo, .6f, float(metallic)),
         "Compressed specular metalness differs from the fork's");
    Case untinted = metal;
    untinted.roughness = sg.c_str();
    untinted.specularTint = {0, 0, 0};
    untinted.lumMax = 1;
    albedo = {.2, .15, .1};
    const double plain = forkMetal(untinted, 204 / 255., albedo);
    same("metal-untinted-" + mode, untinted, constant(albedo, .6f, float(plain)),
         "Untinted specular metalness differs from the fork's");
  };
  auto tints = [&](const std::string &mode) {
    // Tint on gamma values: sRGB views linearize the tint, UNORM views do not.
    const remixapi_Float3D t = {.5f, .8f, 1.2f};
    Case c;
    c.tinted = true;
    c.albedoTint = t;
    c.albedo = albedoSrgb.c_str();
    const double tint[3] = {t.x, t.y, t.z};
    const double texel[3] = {128 / 255., 200 / 255., 64 / 255.};
    std::array<double, 3> expected;
    for (int i = 0; i < 3; ++i)
      expected[i] = std::min(1., srgb(texel[i]) * gamma(tint[i]));
    same("tint-srgb-" + mode, c, constant(expected, .6f, 0), "sRGB albedo tint differs from the fork's");
    c.albedo = albedoUnorm.c_str();
    for (int i = 0; i < 3; ++i)
      expected[i] = std::min(1., texel[i] * tint[i]);
    same("tint-unorm-" + mode, c, constant(expected, .6f, 0), "UNORM albedo tint differs");
    c.albedo = albedoBc1.c_str();
    for (int i = 0; i < 3; ++i)
      expected[i] = std::min(1., srgb(bc1Texel[i]) * gamma(tint[i]));
    same("tint-bc1-" + mode, c, constant(expected, .6f, 0), "Compressed sRGB albedo tint differs");
  };

  pathTracer("raster-primary");
  tints("raster-primary");
  config("primaryVisibility", "ray");
  pathTracer("ray-primary");
  tints("ray-primary");
  config("primaryVisibility", "raster");
  ok(api.SetConfigVariable("rtx.native.backend", "raster"));
  tints("raster-backend");
  ok(api.Shutdown());
  std::puts("Spec/gloss roughness and metalness and albedo tint match the fork's formulas");
  return 0;
}
