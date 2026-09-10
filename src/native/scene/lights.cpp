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
    DistantLight light;
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
    m_lights[info.hash] = light;
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
    if (it->second.isDistant)
      m_light = it->second;
  }
} // namespace remix
