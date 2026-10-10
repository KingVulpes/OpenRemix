// Fork material extensions (remix_fo4_ext.h): spec/gloss roughness maps, albedo
// tint and the per-instance material override, as the dxvk-remix fork parses them.
#include "scene/fixtures.h"
#include "scene/emission.h"
#include "scene/opacity.h"
#include "scene/temporal.h"
#include <chrono>
#include <thread>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>

using namespace remix::test;
namespace scene = remix::scene;

namespace {
  // rtx_scene_manager.cpp (fork): the packed specular-metal parameters.
  uint32_t forkPacked(float strength, float low, float high, float lumMax, remixapi_Float3D tint) {
    auto q = [](float v, float maxCode) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * maxCode + 0.5f); };
    const uint32_t params = q(low, 15) | (q(high, 15) << 4) | (q(lumMax, 15) << 8) | (q(strength, 15) << 12);
    const uint32_t bits = q(tint.x, 31) | (q(tint.y, 63) << 5) | (q(tint.z, 31) << 11);
    return params | (bits << 16);
  }

  void texture(remix::NativeResources &r, uint64_t hash, remixapi_Format format, const uint8_t (&pixel)[4]) {
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.hash = hash;
    info.format = format;
    info.width = info.height = info.depth = info.mipLevels = 1;
    info.data = pixel;
    info.dataSize = 4;
    remixapi_TextureHandle handle;
    r.createTexture(info, handle);
  }

  struct Material {
    remixapi_MaterialInfoOpaqueEXT opaque{};
    remixapi_MaterialInfoOpaqueSpecGlossEXT specGloss{};
    remixapi_MaterialInfoOpaqueAlbedoTintEXT tint{};
    remixapi_MaterialInfo info{};
    Material(uint64_t hash, remixapi_Path albedo, remixapi_Path roughness) {
      opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
      opaque.albedoConstant = {1, 1, 1};
      opaque.opacityConstant = 1;
      opaque.alphaTestType = 7;
      opaque.roughnessConstant = .25f;
      opaque.roughnessTexture = roughness;
      specGloss.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SPEC_GLOSS_EXT;
      specGloss.roughnessEncoding = scene::smoothnessInGreenEncoding;
      specGloss.metalStrength = .8f;
      specGloss.specularMetalLow = .35f;
      specGloss.specularMetalHigh = .65f;
      specGloss.metalAlbedoLuminanceMax = .5f;
      specGloss.specularTint = {.9f, .7f, .2f};
      tint.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_ALBEDO_TINT_EXT;
      tint.albedoTint = {.5f, .75f, 1};
      info.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
      info.hash = hash;
      info.albedoTexture = albedo;
      // The runtime finds each struct anywhere in the chain.
      info.pNext = &opaque;
      opaque.pNext = &specGloss;
      specGloss.pNext = &tint;
    }
  };

  std::shared_ptr<scene::Material> drawn(remix::NativeResources &r, remixapi_MeshHandle mesh) {
    auto info = instance(mesh);
    r.drawInstance(info);
    return r.draws().back().material;
  }

  void halves() {
    require(scene::halfBits(1) == 0x3c00 && scene::halfBits(.5f) == 0x3800 && scene::halfBits(16) == 0x4c00 &&
                scene::halfBits(0) == 0 && scene::halfBits(65504) == 0x7bff && scene::halfBits(1e6f) == 0x7c00 &&
                scene::halfBits(-2) == 0xc000,
            "Half conversion of exact values");
    // 1 + 2^-11 is halfway between 1 and the next half: ties to even (1).
    require(scene::halfBits(1.00048828125f) == 0x3c00 && scene::halfBits(1.00146484375f) == 0x3c02,
            "Half rounding is not nearest-even");
    require(scene::halfBits(std::ldexp(1.f, -24)) == 1 && scene::halfFloat(1) == std::ldexp(1.f, -24),
            "Half subnormals");
    for (unsigned v = 0; v < 256; ++v) {
      const float x = float(v) / 255;
      const float y = scene::halfFloat(scene::halfBits(x));
      require(std::abs(x - y) <= std::ldexp(x, -11) && scene::halfBits(y) == scene::halfBits(x),
              "Half round trip of 8-bit values");
    }
  }

  void specGloss() {
    remix::NativeResources r;
    const uint8_t sg[4] = {200, 64, 0, 255}, albedo[4] = {128, 96, 32, 255};
    texture(r, 0xa, REMIXAPI_FORMAT_R8G8B8A8_UNORM, sg);
    texture(r, 0xb, REMIXAPI_FORMAT_R8G8B8A8_SRGB, albedo);
    texture(r, 0xc, REMIXAPI_FORMAT_R8G8B8A8_UNORM, albedo);
    const auto sgPath = std::filesystem::path("0xa"), srgbPath = std::filesystem::path("0xb"),
               unormPath = std::filesystem::path("0xc");
    Material m(20, srgbPath.c_str(), sgPath.c_str());
    remixapi_MaterialHandle handle;
    r.createMaterial(m.info, handle);
    auto mesh = quad(r, 30, handle);
    auto first = drawn(r, mesh);
    require(first->roughnessEncoding == scene::smoothnessInGreenEncoding && first->roughness == .25f &&
                first->pbr && first->roughnessTexture && !first->roughnessTexture->pixels.srgb,
            "Spec/gloss encoding or roughness floor lost");
    const uint32_t packed = forkPacked(.8f, .35f, .65f, .5f, {.9f, .7f, .2f});
    require(first->specularMetal == packed, "Specular metal parameters differ from the fork's packing");
    auto decode = scene::materialDecode(*first);
    require(decode.flags == (scene::MaterialDecode::roughnessFromSmoothness | scene::MaterialDecode::smoothnessInGreen |
                             scene::MaterialDecode::metallicFromSpecular | scene::MaterialDecode::albedoTint |
                             scene::MaterialDecode::albedoSrgb) &&
                decode.specularMetal == packed,
            "Spec/gloss decode flags");

    // Identity: the same description keeps the material object.
    const auto signature = scene::TemporalHistory::materialSignature(r.draws().back());
    r.createMaterial(m.info, handle);
    require(drawn(r, mesh) == first, "Identical spec/gloss material lost identity");
    auto changed = [&](const char *what) {
      r.createMaterial(m.info, handle);
      auto now = drawn(r, mesh);
      require(now != first && scene::TemporalHistory::materialSignature(r.draws().back()) != signature, what);
      return now;
    };

    // Red smoothness: no metalness (it needs the specular level in red).
    m.specGloss.roughnessEncoding = scene::smoothnessInRedEncoding;
    auto red = changed("Encoding change kept the material");
    require(red->specularMetal == 0 &&
                (scene::materialDecode(*red).flags & 7) == scene::MaterialDecode::roughnessFromSmoothness,
            "Red smoothness derived metalness");
    // Encodings other than smoothness are ignored, as in the fork.
    for (uint32_t encoding : {0u, 1u, 4u, 0xffffffffu}) {
      m.specGloss.roughnessEncoding = encoding;
      r.createMaterial(m.info, handle);
      auto ignored = drawn(r, mesh);
      require(ignored->roughnessEncoding == 0 && ignored->specularMetal == 0 &&
                  (scene::materialDecode(*ignored).flags & 7) == 0,
              "Non-smoothness roughness encoding accepted");
    }
    m.specGloss.roughnessEncoding = scene::smoothnessInGreenEncoding;
    // A strength that quantizes to zero disables metalness.
    m.specGloss.metalStrength = .03f;
    r.createMaterial(m.info, handle);
    require(drawn(r, mesh)->specularMetal == 0, "Zero strength code enabled metalness");
    m.specGloss.metalStrength = .8f;
    // Parameters are clamped to [0, 1] before packing.
    m.specGloss.specularMetalHigh = 7;
    m.specGloss.specularTint = {-1, 2, .5f};
    r.createMaterial(m.info, handle);
    require(drawn(r, mesh)->specularMetal == forkPacked(.8f, .35f, 1, .5f, {0, 1, .5f}),
            "Spec/gloss parameters not clamped like the fork's");
    m.specGloss.specularMetalHigh = .65f;
    m.specGloss.specularTint = {.9f, .7f, .2f};
    // Without a roughness map there is nothing to decode.
    m.opaque.roughnessTexture = nullptr;
    r.createMaterial(m.info, handle);
    auto noMap = drawn(r, mesh);
    require(noMap->specularMetal == 0 && (scene::materialDecode(*noMap).flags & 7) == 0,
            "Spec/gloss decode without a roughness map");
    m.opaque.roughnessTexture = sgPath.c_str();
    // Non-finite parameters are rejected and keep the previous material.
    r.createMaterial(m.info, handle);
    auto good = drawn(r, mesh);
    for (float bad : {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
      m.specGloss.metalAlbedoLuminanceMax = bad;
      rejected([&] { r.createMaterial(m.info, handle); });
      m.specGloss.metalAlbedoLuminanceMax = .5f;
      m.tint.albedoTint.y = bad;
      rejected([&] { r.createMaterial(m.info, handle); });
      m.tint.albedoTint.y = .75f;
    }
    require(drawn(r, mesh) == good, "Rejected edit replaced the material");

    // Albedo tint: half precision, clamped to [0, 16].
    require(good->hasAlbedoTint && good->albedoTint[0] == .5f && good->albedoTint[1] == .75f &&
                good->albedoTint[2] == 1,
            "Albedo tint lost");
    decode = scene::materialDecode(*good);
    require(decode.tint[0] == 0x3800 && decode.tint[1] == 0x3a00 && decode.tint[2] == 0x3c00, "Tint halves");
    m.tint.albedoTint = {-1, 20, 200.f / 255};
    auto clamped = changed("Tint change kept the material");
    require(clamped->albedoTint[0] == 0 && clamped->albedoTint[1] == 16 &&
                clamped->albedoTint[2] == scene::halfFloat(scene::halfBits(200.f / 255)),
            "Albedo tint not clamped / rounded like the fork's");
    // An UNORM albedo is not linearized when sampled: the tint is applied as is.
    m.info.albedoTexture = unormPath.c_str();
    r.createMaterial(m.info, handle);
    require((scene::materialDecode(*drawn(r, mesh)).flags & scene::MaterialDecode::albedoSrgb) == 0,
            "UNORM albedo marked sRGB");
    // (1,1,1) and a missing albedo texture are no-ops.
    m.tint.albedoTint = {1, 1, 1};
    r.createMaterial(m.info, handle);
    require(!drawn(r, mesh)->hasAlbedoTint, "Unit tint applied");
    m.tint.albedoTint = {.5f, .5f, .5f};
    m.info.albedoTexture = nullptr;
    r.createMaterial(m.info, handle);
    auto untextured = drawn(r, mesh);
    require(!untextured->hasAlbedoTint && !(scene::materialDecode(*untextured).flags & scene::MaterialDecode::albedoTint),
            "Tint applied without an albedo texture");

    // Translucent materials ignore both structs (they extend the opaque one).
    remixapi_MaterialInfoTranslucentEXT translucent{};
    translucent.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT;
    translucent.pNext = &m.specGloss;
    translucent.transmittanceColor = {1, 1, 1};
    m.info.pNext = &translucent;
    m.info.albedoTexture = srgbPath.c_str();
    r.createMaterial(m.info, handle);
    auto glass = drawn(r, mesh);
    require(glass->roughnessEncoding == 0 && glass->specularMetal == 0 && !glass->hasAlbedoTint,
            "Translucent material took opaque extensions");
  }

  void materialOverride() {
    remix::NativeResources r;
    auto own = material(r, 1, 1, 0, 0), other = material(r, 2, 0, 1, 0);
    auto mesh = quad(r, 3, own);
    const auto ownMaterial = drawn(r, mesh);
    remixapi_InstanceInfoMaterialOverrideEXT override{};
    override.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_MATERIAL_OVERRIDE_EXT;
    override.material = other;
    remixapi_Transform locals[2] = {};
    for (auto &local : locals)
      local.matrix[0][0] = local.matrix[1][1] = local.matrix[2][2] = 1;
    remixapi_InstanceInfoGpuInstancingEXT instancing{};
    instancing.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT;
    instancing.pNext = &override;
    instancing.instanceTransforms_values = locals;
    instancing.instanceTransforms_count = 2;
    auto info = instance(mesh);
    info.pNext = &instancing;
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws().size() == 2, "Instanced override draw count");
    const auto otherMaterial = r.draws()[0].material;
    require(otherMaterial != ownMaterial && r.draws()[1].material == otherMaterial,
            "DrawInstance ignored the material override");
    // Same chain through a retained instance: the same material.
    const uint64_t retained = r.createRetained(info);
    r.finishFrame();
    r.drawRetained(nullptr);
    require(r.draws().size() == 2 && r.draws()[0].material == otherMaterial &&
                r.draws()[1].material == otherMaterial,
            "Retained and immediate overrides disagree");
    r.destroyRetained(&retained, 1);
    // A null handle keeps the mesh's own material.
    override.material = nullptr;
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws()[0].material == ownMaterial, "Null override replaced the material");
    // A dead handle keeps the mesh's own material, without an error.
    override.material = other;
    r.destroyMaterial(other);
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws().size() == 2 && r.draws()[0].material == ownMaterial,
            "Dead override not ignored");
    // A handle that was never a material is ignored the same way.
    override.material = reinterpret_cast<remixapi_MaterialHandle>(uintptr_t(0x1234));
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws()[0].material == ownMaterial, "Unknown override not ignored");
    // Hidden instances draw nothing with or without the override.
    info.categoryFlags = REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN;
    r.finishFrame();
    r.drawInstance(info);
    require(r.draws().empty(), "Hidden override drawn");
  }
  // remixapi_TextureInfoDynamicEXT / remixapi_UpdateTextureEXT: the texture
  // and what was made from it stay; only the pixels change.
  void dynamicTexture() {
    remix::NativeResources r;
    const uint8_t red[4][4] = {{0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}, {0, 0, 255, 255}}; // B8G8R8A8
    remixapi_TextureInfoDynamicEXT dynamic{};
    dynamic.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_DYNAMIC_EXT;
    dynamic.dynamic = 1;
    remixapi_TextureInfoGenerateMipsEXT mips{};
    mips.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_GENERATE_MIPS_EXT;
    mips.generate = 1;
    remixapi_TextureInfo info{};
    info.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
    info.pNext = &dynamic;
    info.hash = 0x51;
    info.format = REMIXAPI_FORMAT_B8G8R8A8_SRGB;
    info.width = info.height = 2;
    info.depth = info.mipLevels = 1;
    info.data = red;
    info.dataSize = sizeof(red);
    remixapi_TextureHandle handle{}, plain{};
    // One uncompressed level only.
    dynamic.pNext = &mips;
    rejected([&] { r.createTexture(info, handle); });
    dynamic.pNext = nullptr;
    info.format = REMIXAPI_FORMAT_BC1_RGB_SRGB;
    rejected([&] { r.createTexture(info, handle); });
    info.format = REMIXAPI_FORMAT_B8G8R8A8_SRGB;
    r.createTexture(info, handle);
    info.pNext = nullptr;
    info.hash = 0x52;
    r.createTexture(info, plain);

    // An unlit material of it (self-illuminated, emitting), alpha-tested.
    remixapi_MaterialInfoSelfIlluminationEXT self{};
    self.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT;
    self.maskScale = 1;
    remixapi_MaterialInfoOpaqueEXT opaque{};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.pNext = &self;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 4; // greater
    opaque.alphaReferenceValue = 128;
    remixapi_MaterialInfo materialInfo{};
    materialInfo.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    materialInfo.pNext = &opaque;
    materialInfo.hash = 0x53;
#ifdef _WIN32
    materialInfo.albedoTexture = L"0x51";
#else
    materialInfo.albedoTexture = "0x51";
#endif
    materialInfo.emissiveIntensity = 1;
    materialInfo.emissiveColorConstant = {1, 1, 1};
    remixapi_MaterialHandle material{};
    r.createMaterial(materialInfo, material);
    const auto mesh = quad(r, 0x54, material);
    auto info2 = instance(mesh);
    info2.doubleSided = 1;
    r.drawInstance(info2);
    const scene::Draw draw = r.draws().back();
    const auto texture = draw.material->texture;
    require(texture->dynamic && texture->updates && texture->pixels.mips.size() == 1 &&
                texture->pixels.mips[0][0] == 255 && texture->pixels.mips[0][2] == 0,
            "Dynamic texture creation");
    const uint64_t fingerprint = texture->fingerprint(), signature = scene::TemporalHistory::materialSignature(draw);

    // Estimates of the first image.
    scene::SurfaceEmissionCache emission;
    const std::array<float, 4> uv{1, 1, 0, 0};
    const auto first = emission.reduce(draw, uv);
    require(first->size() == 2 && (*first)[0].radiance[0] > .9f && (*first)[0].radiance[1] == 0,
            "Dynamic texture emission of the first image");
    scene::OpacityCache opacity;
    const auto partition = opacity.partition(draw, uv);
    require(partition->reasons[scene::OpacityPartition::Solid] == 0 &&
                partition->reasons[scene::OpacityPartition::TextureCutout] == 2,
            "A dynamic texture's opaque texels were proven opaque");

    // The same pixels: no update. Other handles, regions and formats: rejected.
    r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 8);
    require(texture->updates->revision == 0, "An unchanged image counted as an update");
    rejected([&] { r.updateTexture(plain, 0, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 8); });
    rejected([&] { r.updateTexture(handle, 1, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 8); });
    rejected([&] { r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_BC1_RGB_SRGB, red, 8); });
    rejected([&] { r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 4); });

    // The bottom row turns green (R8G8B8A8 this time, with a row pitch).
    const uint8_t rows[2][12] = {{255, 0, 0, 255, 255, 0, 0, 255, 9, 9, 9, 9}, {0, 255, 0, 255, 0, 255, 0, 255, 9, 9, 9, 9}};
    r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_R8G8B8A8_UNORM, rows, 12);
    {
      std::lock_guard lock(texture->updates->lock);
      const auto &u = *texture->updates;
      require(u.revision == 1 && u.dirty[0] == 0 && u.dirty[1] == 1 && u.dirty[2] == 2 && u.dirty[3] == 2,
              "Changed region of an update");
      const uint8_t expected[16] = {255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0, 255};
      require(std::memcmp(texture->pixels.mips[0].data(), expected, 16) == 0, "Updated pixels");
    }
    // Nothing else changed: the same texture and material objects, the same
    // fingerprint and material signature.
    r.finishFrame();
    r.drawInstance(info2);
    require(r.draws().back().material == draw.material && r.draws().back().material->texture == texture &&
                texture->fingerprint() == fingerprint &&
                scene::TemporalHistory::materialSignature(r.draws().back()) == signature && texture->pixels.srgb,
            "An update changed the texture's identity");
    // The emissive estimate is the first image's until it is old enough, for
    // this surface and for a new one of the same material.
    require(emission.reduce(draw, uv) == first, "Emission was estimated again at once");
    scene::Draw other = draw;
    other.surface = std::make_shared<scene::Surface>(*draw.surface);
    const auto fresh = emission.reduce(other, uv);
    require(fresh->size() == 2 && (*fresh)[0].radiance == (*first)[0].radiance && (*fresh)[1].radiance == (*first)[1].radiance,
            "A new surface did not keep the material's estimate");
    std::this_thread::sleep_for(std::chrono::milliseconds(int(REMIXAPI_DYNAMIC_TEXTURE_EMISSION_SECONDS_EXT * 1000) + 50));
    const auto second = emission.reduce(draw, uv);
    float sum[3] = {};
    for (const auto &triangle : *second)
      for (int c = 0; c < 3; ++c)
        sum[c] += triangle.radiance[c];
    require(second != first && sum[0] > .5f && sum[1] > .5f && sum[2] == 0, "Emission did not follow the new image");
    require(emission.reduce(draw, uv) == second, "Emission of an unchanged image was estimated again");
    // A direct estimate reads the current pixels.
    r.updateTexture(handle, 0, 0, 2, 2, REMIXAPI_FORMAT_B8G8R8A8_SRGB, red, 8);
    const float transform[4] = {1, 1, 0, 0};
    const auto direct = scene::triangleEmission(draw, 0, transform);
    require(direct[0] > .9f && direct[1] == 0, "Direct emission estimate of a dynamic texture");
    // CreateTexture under the hash is a new texture; the material keeps its own.
    info.pNext = &dynamic;
    info.hash = 0x51;
    r.createTexture(info, handle);
    r.finishFrame();
    r.drawInstance(info2);
    require(r.draws().back().material->texture == texture, "CreateTexture replaced a material's dynamic texture");
  }
} // namespace

int main() {
  halves();
  specGloss();
  materialOverride();
  dynamicTexture();
  std::puts("Spec/gloss, albedo tint and material override passed");
  return 0;
}
