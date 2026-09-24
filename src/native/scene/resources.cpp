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

  NativeResources::NativeResources() { reset(); }

  void NativeResources::reset() {
    m_draws.clear();
    m_frameIds.clear();
    m_uiDraws.clear();
    m_overlay.clear();
    m_materials.clear();
    m_meshes.clear();
    m_textures.clear();
    m_uiTextures.clear();
    m_paths.clear();
    m_white.reset();
    m_default.reset();
    m_cameraValid = {};
    m_temporalEpoch = {};
    m_skyRadiance = {};
    m_lights.clear();
    m_light = {};
    m_frameLights.clear();
    for (auto &view : m_views)
      view = identity();
    for (auto &camera : m_cameras) {
      camera = identity();
    }
    for (auto &depth : m_reverseDepth)
      depth = {0, 0, -1, 1};
    std::fill(std::begin(m_tint), std::end(m_tint), 0.0f);
  }

  void NativeResources::finishFrame() {
    m_draws.clear();
    m_frameIds.clear();
    m_uiDraws.clear();
    m_light = {};
    m_frameLights.clear();
  }
} // namespace remix
