#include "scene/resources.h"
#include "scene/images.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <utility>

namespace remix {
  using namespace scene;
  using namespace scene::detail;

  NativeResources::NativeResources() { reset(); }

  void NativeResources::reset() {
    m_frame = {};
    m_submitted = {};
    m_materials.clear();
    m_modMaterials.clear();
    m_meshes.clear();
    m_textures.clear();
    m_uiTextures.clear();
    m_paths.clear();
    m_lights.clear();
    // Created eagerly: rendering may read these while API calls run.
    m_white.reset();
    m_default.reset();
    texturePath(nullptr);
    defaultMaterial();
  }

  void NativeResources::finishFrame() {
    // Per-frame lists swap to keep their capacity for the next frame. Cameras,
    // epochs, overlay and tint persist until replaced; lights are per frame.
    std::swap(m_submitted.draws, m_frame.draws);
    std::swap(m_submitted.uiDraws, m_frame.uiDraws);
    std::swap(m_submitted.ids, m_frame.ids);
    std::swap(m_submitted.lights, m_frame.lights);
    m_frame.draws.clear();
    m_frame.uiDraws.clear();
    m_frame.ids.clear();
    m_frame.lights.clear();
    m_submitted.light = std::exchange(m_frame.light, {});
    m_submitted.dome = std::exchange(m_frame.dome, {});
    m_submitted.overlay = m_frame.overlay;
    m_submitted.cameras = m_frame.cameras;
    m_submitted.views = m_frame.views;
    m_submitted.projections = m_frame.projections;
    m_submitted.reverseDepth = m_frame.reverseDepth;
    m_submitted.cameraValid = m_frame.cameraValid;
    m_submitted.temporalEpoch = m_frame.temporalEpoch;
    m_submitted.skyRadiance = m_frame.skyRadiance;
    std::copy_n(m_frame.tint, 4, m_submitted.tint);
  }
} // namespace remix
