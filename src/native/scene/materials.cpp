#include "scene/images.h"
#include "scene/resources.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace remix::scene {
  uint16_t halfBits(float v) {
    uint32_t f;
    std::memcpy(&f, &v, sizeof(f));
    const uint32_t sign = (f >> 16) & 0x8000u;
    f &= 0x7fffffffu;
    if (f >= 0x47800000u) // 65536 and above (the caller passes finite values)
      return uint16_t(sign | 0x7c00u);
    if (f < 0x38800000u) { // below the smallest normal half: subnormal steps of 2^-24
      float a;
      std::memcpy(&a, &f, sizeof(a));
      return uint16_t(sign | uint32_t(std::nearbyint(a * 16777216.0f)));
    }
    const uint32_t half = (f - 0x38000000u) >> 13, rest = f & 0x1fffu;
    return uint16_t(sign | (half + uint32_t(rest > 0x1000u || (rest == 0x1000u && (half & 1u)))));
  }

  float halfFloat(uint16_t bits) {
    const uint32_t exponent = (bits >> 10) & 31u, mantissa = bits & 0x3ffu;
    float v;
    if (exponent == 0)
      v = std::ldexp(float(mantissa), -24);
    else if (exponent == 31)
      v = mantissa ? std::numeric_limits<float>::quiet_NaN() : std::numeric_limits<float>::infinity();
    else {
      const uint32_t f = ((exponent + 112u) << 23) | (mantissa << 13);
      std::memcpy(&v, &f, sizeof(v));
    }
    return (bits & 0x8000u) ? -v : v;
  }

  MaterialDecode materialDecode(const Material &m) {
    MaterialDecode d;
    if (m.roughnessTexture && (m.roughnessEncoding == smoothnessInRedEncoding ||
                               m.roughnessEncoding == smoothnessInGreenEncoding)) {
      d.flags |= MaterialDecode::roughnessFromSmoothness;
      if (m.roughnessEncoding == smoothnessInGreenEncoding) {
        d.flags |= MaterialDecode::smoothnessInGreen;
        if (m.specularMetal) {
          d.flags |= MaterialDecode::metallicFromSpecular;
          d.specularMetal = m.specularMetal;
        }
      }
    }
    if (m.hasAlbedoTint) {
      d.flags |= MaterialDecode::albedoTint;
      // An sRGB view is already linear when sampled, so the shader linearizes
      // the tint to match (fork: OPAQUE_SURFACE_MATERIAL_FLAG_ALBEDO_TEXTURE_IS_SRGB).
      const auto &p = m.texture->pixels;
      if (p.srgb && p.hdrMips.empty())
        d.flags |= MaterialDecode::albedoSrgb;
      for (unsigned c = 0; c < 3; ++c)
        d.tint[c] = halfBits(m.albedoTint[c]);
    }
    if (m.selfIllumination) {
      d.flags |= MaterialDecode::selfIllumination |
                 uint32_t(m.selfIllumination - 1) << MaterialDecode::selfIlluminationMaskShift;
      d.selfIlluminationScale = m.selfIlluminationScale;
    }
    return d;
  }
} // namespace remix::scene

namespace remix {
  using namespace scene;
  using namespace scene::detail;

  namespace {
    // The dxvk-remix fork's opaque-material extensions, with its encodings,
    // quantization and clamps (rtx_remix_api.cpp / rtx_scene_manager.cpp).
    void parseFork(const remixapi_MaterialInfo &info, Material &material) {
      if (const auto *sg = static_cast<const remixapi_MaterialInfoOpaqueSpecGlossEXT *>(
              extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_SPEC_GLOSS_EXT))) {
        const float values[] = {sg->metalStrength,   sg->specularMetalLow, sg->specularMetalHigh,
                                sg->metalAlbedoLuminanceMax, sg->specularTint.x, sg->specularTint.y,
                                sg->specularTint.z};
        finite(values, std::size(values));
        // Only the smoothness encodings mean anything for a roughness map.
        if (sg->roughnessEncoding == smoothnessInRedEncoding ||
            sg->roughnessEncoding == smoothnessInGreenEncoding)
          material.roughnessEncoding = sg->roughnessEncoding;
        // Metalness needs gloss in green (red = specular level) and a map.
        auto q = [](float v, float maxCode) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * maxCode + 0.5f); };
        const uint32_t strength = q(sg->metalStrength, 15);
        if (sg->metalStrength > 0 && strength &&
            material.roughnessEncoding == smoothnessInGreenEncoding && material.roughnessTexture) {
          const uint32_t params = q(sg->specularMetalLow, 15) | q(sg->specularMetalHigh, 15) << 4 |
                                  q(sg->metalAlbedoLuminanceMax, 15) << 8 | strength << 12;
          const uint32_t tint = q(sg->specularTint.x, 31) | q(sg->specularTint.y, 63) << 5 |
                                q(sg->specularTint.z, 31) << 11;
          material.specularMetal = params | tint << 16;
        }
      }
      if (const auto *at = static_cast<const remixapi_MaterialInfoOpaqueAlbedoTintEXT *>(
              extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_ALBEDO_TINT_EXT))) {
        finite(&at->albedoTint.x, 3);
        float tint[3];
        for (unsigned c = 0; c < 3; ++c)
          tint[c] = halfFloat(halfBits(std::clamp((&at->albedoTint.x)[c], 0.0f, 16.0f)));
        // Only meaningful with an albedo texture; (1,1,1) is a no-op.
        if (info.albedoTexture && *info.albedoTexture && (tint[0] != 1 || tint[1] != 1 || tint[2] != 1)) {
          material.hasAlbedoTint = true;
          std::copy_n(tint, 3, material.albedoTint);
        }
      }
    }
  } // namespace

  void NativeResources::createMaterial(const remixapi_MaterialInfo &info, remixapi_MaterialHandle &handle) {
    if (!info.hash) {
      throw invalid;
    }
    auto material = std::make_shared<Material>();
    if (auto *normal = static_cast<const remixapi_MaterialNormalEncodingEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_NORMAL_ENCODING_EXT))) {
      if (normal->encoding > 3)
        throw invalid;
      material->normalEncoding = normal->encoding;
    }
    const auto *opaque = static_cast<const remixapi_MaterialInfoOpaqueEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT));
    const auto *translucent = static_cast<const remixapi_MaterialInfoTranslucentEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_TRANSLUCENT_EXT));
    const auto *portal = extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_PORTAL_EXT);
    if (unsigned(opaque != nullptr) + unsigned(translucent != nullptr) + unsigned(portal != nullptr) != 1) {
      throw invalid;
    }
    remixapi_Path texture = info.albedoTexture;
    if (opaque) {
      finite(&opaque->roughnessConstant, 1);
      finite(&opaque->metallicConstant, 1);
      if (opaque->roughnessConstant < 0 || opaque->roughnessConstant > 1 || opaque->metallicConstant < 0 ||
          opaque->metallicConstant > 1)
        throw invalid;
      material->roughness = opaque->roughnessConstant;
      material->metallic = opaque->metallicConstant;
      material->roughnessTexture = dataTexturePath(opaque->roughnessTexture);
      material->metallicTexture = dataTexturePath(opaque->metallicTexture);
      material->tint[0] = opaque->albedoConstant.x;
      material->tint[1] = opaque->albedoConstant.y;
      material->tint[2] = opaque->albedoConstant.z;
      material->tint[3] = opaque->opacityConstant;
      if (opaque->alphaTestType < 0 || opaque->alphaTestType > 7) {
        throw invalid;
      }
      material->compare = uint32_t(opaque->alphaTestType);
      material->reference = float(opaque->alphaReferenceValue) / 255;
      material->drawAlpha = opaque->useDrawCallAlphaState != 0;
      material->translucent = opaque->opacityConstant < 1;
      parseFork(info, *material);
    } else if (translucent) {
      material->tint[0] = translucent->transmittanceColor.x;
      material->tint[1] = translucent->transmittanceColor.y;
      material->tint[2] = translucent->transmittanceColor.z;
      // Deliberate raster approximation: texture alpha and transmission tint, no refraction.
      if ((!texture || !*texture) && translucent->transmittanceTexture) {
        texture = translucent->transmittanceTexture;
      }
      material->translucent = true;
      material->transmission = true;
    }
    finite(material->tint, 4);
    if (material->tint[3] < 0 || material->tint[3] > 1) {
      throw invalid;
    }
    if (info.filterMode > 1 || info.wrapModeU > 4 || info.wrapModeV > 4) {
      throw unavailable;
    }
    material->filter = info.filterMode;
    material->wrapU = info.wrapModeU;
    material->wrapV = info.wrapModeV;
    material->rows = std::max(uint8_t(1), info.spriteSheetRow);
    material->columns = std::max(uint8_t(1), info.spriteSheetCol);
    material->fps = info.spriteSheetFps;
    material->texture = texturePath(texture);
    if (material->texture && !material->sourceHash) {
      material->sourceHash = material->texture->getHash();
    }
    material->normalTexture = dataTexturePath(info.normalTexture);
    const auto *pbr = static_cast<const remixapi_MaterialInfoPbrEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT));
    if (pbr && (!opaque || pbr->enabled > 1))
      throw invalid;
    material->pbr =
        opaque && (pbr ? pbr->enabled != 0
                       : material->roughness > 0 || material->metallic > 0 || material->roughnessTexture ||
                             material->metallicTexture || material->normalTexture);
    finite(&info.emissiveColorConstant.x, 3);
    finite(&info.emissiveIntensity, 1);
    if (info.emissiveIntensity < 0 || info.emissiveColorConstant.x < 0 || info.emissiveColorConstant.y < 0 ||
        info.emissiveColorConstant.z < 0)
      throw invalid;
    for (unsigned c = 0; c < 3; ++c)
      material->emission[c] = (&info.emissiveColorConstant.x)[c] * info.emissiveIntensity;
    finite(material->emission, 3);
    if (const auto *emission = static_cast<const remixapi_MaterialInfoEmissionEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT))) {
      if (emission->suppressLightGeneration > 1)
        throw invalid;
      material->suppressEmissionLight = emission->suppressLightGeneration != 0;
    }
    const auto *self = static_cast<const remixapi_MaterialInfoSelfIlluminationEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_SELF_ILLUMINATION_EXT));
    const bool emissionMap = info.emissiveTexture && *info.emissiveTexture;
    if (self) {
      if (!opaque || self->mask > REMIXAPI_SELF_ILLUMINATION_MASK_EMISSIVE_ALPHA_EXT ||
          !std::isfinite(self->maskScale) || !(self->maskScale > 0) ||
          (self->mask == REMIXAPI_SELF_ILLUMINATION_MASK_EMISSIVE_ALPHA_EXT && !emissionMap))
        throw invalid;
      material->selfIllumination = uint8_t(self->mask + 1);
      material->selfIlluminationScale = self->maskScale;
    }
    // A self-illumination mask holds values, not colours.
    if (emissionMap)
      material->emissionTexture = self ? dataTexturePath(info.emissiveTexture) : texturePath(info.emissiveTexture);
    auto previous = m_materials.find(info.hash);
    // A render thread may be filling the previous material's cache: it is
    // only carried over while frames render here (a CPU cache; the replacement
    // computes the same values on demand).
    if (!m_concurrentConsumer && previous != m_materials.end() &&
        previous->second->emissionTexture == material->emissionTexture &&
        previous->second->selfIllumination == material->selfIllumination &&
        previous->second->selfIlluminationScale == material->selfIlluminationScale &&
        (!material->selfIllumination || (previous->second->texture == material->texture &&
                                         std::equal(material->tint, material->tint + 3, previous->second->tint))) &&
        std::equal(material->emission, material->emission + 3, previous->second->emission) &&
        material->wrapU == previous->second->wrapU && material->wrapV == previous->second->wrapV)
      material->emissionCache = previous->second->emissionCache;
    // remixapi_MaterialInfoReplacementHashEXT: the key USD mods replace it by.
    if (const auto *replacement = static_cast<const remixapi_MaterialInfoReplacementHashEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_REPLACEMENT_HASH_EXT)))
      material->sourceHash = replacement->materialHash;
    auto equal = [&](const Material &a, const Material &b) {
      return a.sourceHash == b.sourceHash && a.texture == b.texture && a.emissionTexture == b.emissionTexture &&
             a.suppressEmissionLight == b.suppressEmissionLight && a.selfIllumination == b.selfIllumination &&
             a.selfIlluminationScale == b.selfIlluminationScale && a.roughnessTexture == b.roughnessTexture &&
             a.metallicTexture == b.metallicTexture && a.normalTexture == b.normalTexture &&
             a.normalEncoding == b.normalEncoding && a.roughness == b.roughness && a.metallic == b.metallic &&
             a.pbr == b.pbr && std::equal(a.emission, a.emission + 3, b.emission) &&
             std::equal(a.tint, a.tint + 4, b.tint) && a.compare == b.compare && a.reference == b.reference &&
             a.drawAlpha == b.drawAlpha && a.translucent == b.translucent &&
             a.transmission == b.transmission && a.filter == b.filter && a.wrapU == b.wrapU &&
             a.wrapV == b.wrapV && a.rows == b.rows && a.columns == b.columns && a.fps == b.fps &&
             a.roughnessEncoding == b.roughnessEncoding && a.specularMetal == b.specularMetal &&
             a.hasAlbedoTint == b.hasAlbedoTint && std::equal(a.albedoTint, a.albedoTint + 3, b.albedoTint);
    };
    if (previous == m_materials.end() || !equal(*previous->second, *material)) {
      if (previous != m_materials.end()) {
        previous->second->retired = true;
        ++m_retireGeneration;
      }
      m_materials[info.hash] = std::move(material);
      ++m_resourceGeneration;
      textureCounters.materialsCreated.fetch_add(1, std::memory_order_relaxed);
    }
    handle = reinterpret_cast<remixapi_MaterialHandle>(uintptr_t(info.hash));
  }

  void NativeResources::destroyMaterial(remixapi_MaterialHandle handle) {
    if (auto it = m_materials.find(key(handle)); it != m_materials.end()) {
      it->second->retired = true;
      ++m_retireGeneration;
      m_materials.erase(it);
      textureCounters.materialsDestroyed.fetch_add(1, std::memory_order_relaxed);
    }
  }
} // namespace remix
