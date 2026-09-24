#include "scene/resources.h"
#include "scene/images.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace remix {
  using namespace scene;
  using namespace scene::detail;

  void NativeResources::createMaterial(const remixapi_MaterialInfo &info, remixapi_MaterialHandle &handle) {
    if (!info.hash) {
      throw invalid;
    }
    auto material = std::make_shared<Material>();
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
      if (opaque->roughnessConstant < 0 || opaque->roughnessConstant > 1 ||
          opaque->metallicConstant < 0 || opaque->metallicConstant > 1) throw invalid;
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
    material->normalTexture = dataTexturePath(info.normalTexture);
    const auto *pbr = static_cast<const remixapi_MaterialInfoPbrEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_PBR_EXT));
    if (pbr && (!opaque || pbr->enabled > 1)) throw invalid;
    material->pbr = opaque && (pbr ? pbr->enabled != 0 :
        material->roughness > 0 || material->metallic > 0 || material->roughnessTexture ||
        material->metallicTexture || material->normalTexture);
    finite(&info.emissiveColorConstant.x, 3);
    finite(&info.emissiveIntensity, 1);
    if (info.emissiveIntensity < 0 || info.emissiveColorConstant.x < 0 ||
        info.emissiveColorConstant.y < 0 || info.emissiveColorConstant.z < 0)
      throw invalid;
    for (unsigned c = 0; c < 3; ++c)
      material->emission[c] = (&info.emissiveColorConstant.x)[c] * info.emissiveIntensity;
    finite(material->emission, 3);
    if (const auto *emission = static_cast<const remixapi_MaterialInfoEmissionEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_MATERIAL_EMISSION_EXT))) {
      if (emission->suppressLightGeneration > 1) throw invalid;
      material->suppressEmissionLight = emission->suppressLightGeneration != 0;
    }
    if (info.emissiveTexture && *info.emissiveTexture)
      material->emissionTexture = texturePath(info.emissiveTexture);
    auto previous = m_materials.find(info.hash);
    if (previous != m_materials.end() && previous->second->emissionTexture == material->emissionTexture &&
        std::equal(material->emission, material->emission + 3, previous->second->emission) &&
        material->wrapU == previous->second->wrapU && material->wrapV == previous->second->wrapV)
      material->emissionCache = previous->second->emissionCache;
    auto equal = [&](const Material &a, const Material &b) {
      return a.texture == b.texture && a.emissionTexture == b.emissionTexture &&
             a.suppressEmissionLight == b.suppressEmissionLight &&
             a.roughnessTexture == b.roughnessTexture && a.metallicTexture == b.metallicTexture &&
             a.normalTexture == b.normalTexture && a.roughness == b.roughness &&
             a.metallic == b.metallic && a.pbr == b.pbr &&
             std::equal(a.emission, a.emission + 3, b.emission) &&
             std::equal(a.tint, a.tint + 4, b.tint) && a.compare == b.compare &&
             a.reference == b.reference && a.drawAlpha == b.drawAlpha && a.translucent == b.translucent &&
             a.transmission == b.transmission && a.filter == b.filter && a.wrapU == b.wrapU &&
             a.wrapV == b.wrapV && a.rows == b.rows && a.columns == b.columns && a.fps == b.fps;
    };
    if (previous == m_materials.end() || !equal(*previous->second, *material))
      m_materials[info.hash] = std::move(material);
    handle = reinterpret_cast<remixapi_MaterialHandle>(uintptr_t(info.hash));
  }

  void NativeResources::destroyMaterial(remixapi_MaterialHandle handle) { m_materials.erase(key(handle)); }
} // namespace remix
