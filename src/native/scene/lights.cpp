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

  void NativeResources::createLight(const remixapi_LightInfo &info, remixapi_LightHandle &handle) {
    if (!info.hash)
      throw invalid;
    bool known = false;
    for (auto type : {REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT,
                      REMIXAPI_STRUCT_TYPE_LIGHT_INFO_RECT_EXT, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISK_EXT,
                      REMIXAPI_STRUCT_TYPE_LIGHT_INFO_CYLINDER_EXT,
                      REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DOME_EXT})
      known |= extension(info.pNext, type) != nullptr;
    if (!known)
      throw invalid;
    LightDefinition definition;
    auto &light = definition.distant;
    const auto *distant = static_cast<const remixapi_LightInfoDistantEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_DISTANT_EXT));
    // USD definitions retain their documented compatibility behavior.
    if (distant && !extension(info.pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT)) {
      light.isDistant = true;
      const float direction[] = {distant->direction.x, distant->direction.y, distant->direction.z};
      const float radiance[] = {info.radiance.x, info.radiance.y, info.radiance.z};
      finite(direction, 3);
      finite(radiance, 3);
      const float diameter = distant->angularDiameterDegrees;
      if (!std::isfinite(diameter) || diameter < 0 || diameter >= 180)
        throw invalid;
      light.angularDiameterDegrees = diameter;
      float norm = std::hypot(direction[0], direction[1], direction[2]);
      if (!std::isfinite(norm) || norm < 1e-6f)
        throw invalid;
      for (int i = 0; i < 3; ++i) {
        if (radiance[i] < 0)
          throw invalid;
        light.direction[i] = direction[i] / norm;
        light.radiance[i] = radiance[i];
      }
    }
    const auto *sphere = static_cast<const remixapi_LightInfoSphereEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT));
    if (sphere && !distant && !extension(info.pNext, REMIXAPI_STRUCT_TYPE_LIGHT_INFO_USD_EXT)) {
      const float position[] = {sphere->position.x, sphere->position.y, sphere->position.z};
      const float radiance[] = {info.radiance.x, info.radiance.y, info.radiance.z};
      finite(position, 3);
      finite(radiance, 3);
      if (!std::isfinite(sphere->radius) || sphere->radius < 0 || sphere->radius > 1.e6f ||
          sphere->shaping_hasvalue)
        throw invalid;
      for (unsigned c = 0; c < 3; ++c)
        if (std::abs(position[c]) > 1.e12f || radiance[c] < 0 || radiance[c] > 1.e12f)
          throw invalid;
      // Retain the SDK's zero-radius compatibility placeholder (zero area, no light).
      definition.sphere.id = sphere->radius > 0 ? info.hash : 0;
      definition.sphere.radius = sphere->radius;
      std::copy_n(position, 3, definition.sphere.position);
      std::copy_n(radiance, 3, definition.sphere.radiance);
    }
    m_lights[info.hash] = definition;
    handle = reinterpret_cast<remixapi_LightHandle>(uintptr_t(info.hash));
  }

  void NativeResources::updateLight(remixapi_LightHandle handle, const remixapi_LightInfo &info) {
    if (!m_lights.count(key(handle)))
      throw invalid;
    auto replacement = info;
    replacement.hash = key(handle);
    createLight(replacement, handle);
  }

  void NativeResources::destroyLight(remixapi_LightHandle handle) { m_lights.erase(key(handle)); }

  void NativeResources::drawLight(remixapi_LightHandle handle) {
    const auto it = m_lights.find(key(handle));
    if (it == m_lights.end())
      throw invalid;
    // Last distant light wins, including black lights; compatibility types do not deselect it.
    if (it->second.distant.isDistant)
      m_light = it->second.distant;
    if (it->second.sphere.id)
      m_frameLights.insert(key(handle));
  }
  std::vector<SphereLight> NativeResources::sphereLights() const {
    std::vector<SphereLight> result;
    result.reserve(m_frameLights.size());
    for (const auto id : m_frameLights) {
      const auto it = m_lights.find(id);
      if (it != m_lights.end() && it->second.sphere.id)
        result.push_back(it->second.sphere);
    }
    return result;
  }
} // namespace remix
