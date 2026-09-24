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

  void NativeResources::setupCamera(const remixapi_CameraInfo &info) {
    if (info.type < REMIXAPI_CAMERA_TYPE_WORLD || info.type > REMIXAPI_CAMERA_TYPE_VIEW_MODEL) {
      throw invalid;
    }
    Matrix view, projection;
    if (const auto *p = static_cast<const remixapi_CameraInfoParameterizedEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_CAMERA_INFO_PARAMETERIZED_EXT))) {
      const float vectors[] = {p->position.x, p->position.y, p->position.z, p->forward.x,
                               p->forward.y,  p->forward.z,  p->up.x,       p->up.y,
                               p->up.z,       p->right.x,    p->right.y,    p->right.z};
      finite(vectors, 12);
      if (!std::isfinite(p->aspect) || p->aspect <= 0 || !std::isfinite(p->nearPlane) || p->nearPlane <= 0 ||
          !std::isfinite(p->farPlane) || p->farPlane <= p->nearPlane || !std::isfinite(p->fovYInDegrees) ||
          p->fovYInDegrees <= 0 || p->fovYInDegrees >= 180) {
        throw invalid;
      }
      float basis[3][3] = {{p->right.x, p->right.y, p->right.z},
                           {p->up.x, p->up.y, p->up.z},
                           {p->forward.x, p->forward.y, p->forward.z}};
      for (auto &axis : basis) {
        const float norm = std::hypot(axis[0], axis[1], axis[2]);
        if (!std::isfinite(norm) || norm < 1e-6f) {
          throw invalid;
        }
        for (float &v : axis) {
          v /= norm;
        }
      }
      float inverse[3][3];
      for (int r = 0; r < 3; ++r) {
        const auto &a = basis[(r + 1) % 3], &b = basis[(r + 2) % 3];
        inverse[r][0] = a[1] * b[2] - a[2] * b[1];
        inverse[r][1] = a[2] * b[0] - a[0] * b[2];
        inverse[r][2] = a[0] * b[1] - a[1] * b[0];
      }
      const float det =
          basis[0][0] * inverse[0][0] + basis[0][1] * inverse[0][1] + basis[0][2] * inverse[0][2];
      if (std::abs(det) < 1e-5f) {
        throw invalid;
      }
      const float position[3] = {p->position.x, p->position.y, p->position.z};
      view = identity();
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
          view[c * 4 + r] = inverse[r][c] / det;
          view[12 + r] -= view[c * 4 + r] * position[c];
        }
      }
      const float y = 1 / std::tan(p->fovYInDegrees * 0.00872664625997165f);
      projection = {};
      projection[0] = y / p->aspect;
      projection[5] = y;
      projection[10] = p->farPlane / (p->farPlane - p->nearPlane);
      projection[11] = 1;
      projection[14] = -p->nearPlane * projection[10];
    } else {
      std::memcpy(view.data(), info.view, sizeof(view));
      std::memcpy(projection.data(), info.projection, sizeof(projection));
      finite(view.data(), 16);
      finite(projection.data(), 16);
    }
    auto camera = multiply(projection, view);
    for (int c = 0; c < 4; ++c) {
      camera[c * 4 + 1] = -camera[c * 4 + 1];
    } // Vulkan framebuffer Y.
    finite(camera.data(), 16);
    // Reverse clip depth before multiplying by the view matrix. Subtracting
    // already-rounded clip Z from W in the shader would retain the precision
    // loss that makes nearby layers (such as grass-side overlays) fight.
    std::array<float, 4> reverseDepth{};
    for (int c = 0; c < 4; ++c) {
      double value = 0;
      for (int k = 0; k < 4; ++k)
        value += (double(projection[k * 4 + 3]) - projection[k * 4 + 2]) * view[c * 4 + k];
      reverseDepth[c] = float(value);
    }
    finite(reverseDepth.data(), reverseDepth.size());
    // Validate every extension before committing any camera or world state.
    std::array<float, 3> sky = {};
    if (const auto *environment = static_cast<const remixapi_CameraEnvironmentEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_CAMERA_ENVIRONMENT_EXT))) {
      const auto &rgb = environment->radiance;
      sky = {rgb.x, rgb.y, rgb.z};
      finite(sky.data(), sky.size());
      if (environment->enabled > 1 || std::any_of(sky.begin(), sky.end(), [](float v) { return v < 0; }))
        throw invalid;
      if (!environment->enabled)
        sky = {};
    }
    if (info.type == REMIXAPI_CAMERA_TYPE_WORLD) {
      const auto *metadata = static_cast<const remixapi_CameraTemporalEXT *>(
          extension(info.pNext, REMIXAPI_STRUCT_TYPE_CAMERA_TEMPORAL_EXT));
      m_temporalEpoch =
          metadata ? TemporalEpoch{metadata->worldEpoch, metadata->historyResetEpoch} : TemporalEpoch{};
      m_skyRadiance = sky;
    }
    m_cameras[info.type] = camera;
    m_views[info.type] = view;
    m_reverseDepth[info.type] = reverseDepth;
    m_cameraValid[info.type] = true;
  }

  void NativeResources::drawInstance(const remixapi_InstanceInfo &info) {
    auto mesh = m_meshes.find(key(info.mesh));
    if (mesh == m_meshes.end()) {
      throw invalid;
    }
    auto model = transform(info.transform);
    if (info.categoryFlags & (REMIXAPI_INSTANCE_CATEGORY_BIT_HIDDEN | REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE |
                              REMIXAPI_INSTANCE_CATEGORY_BIT_FIRST_PERSON_PLAYER_SHADOW)) {
      return;
    }
    const auto *alpha = static_cast<const remixapi_InstanceInfoBlendEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BLEND_EXT));
    const auto state = blend(alpha);
    const auto *identity = static_cast<const remixapi_InstanceIdentityEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_IDENTITY_EXT));
    if (identity && (identity->classification > 2 || identity->rasterVisible > 1 ||
                     (!identity->instanceId && identity->classification != 2)))
      throw invalid;
    const bool traceIdentity = identity && identity->classification != REMIXAPI_INSTANCE_RASTER_ONLY_EXT &&
                               !(info.categoryFlags & (REMIXAPI_INSTANCE_CATEGORY_BIT_SKY |
                                                       REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL));
    if (traceIdentity && m_frameIds.count(identity->instanceId))
      throw invalid;
    const auto *rasterState = static_cast<const remixapi_InstanceRasterStateEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_RASTER_STATE_EXT));
    if (rasterState && (rasterState->depthTest > 1 || rasterState->depthWrite > 1 ||
                        rasterState->depthCompare > 7 || rasterState->disableColorWrites > 1)) {
      throw invalid;
    }
    if (alpha && alpha->alphaTestEnabled && alpha->alphaTestCompareOp > 7) {
      throw invalid;
    }
    std::shared_ptr<std::vector<Matrix>> bones;
    if (const auto *skin = static_cast<const remixapi_InstanceInfoBoneTransformsEXT *>(
            extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_BONE_TRANSFORMS_EXT))) {
      if (skin->boneTransforms_count > 256 || (skin->boneTransforms_count && !skin->boneTransforms_values)) {
        throw invalid;
      }
      bones = std::make_shared<std::vector<Matrix>>();
      for (uint32_t i = 0; i < skin->boneTransforms_count; ++i) {
        bones->push_back(transform(skin->boneTransforms_values[i]));
      }
    }
    const auto *instances = static_cast<const remixapi_InstanceInfoGpuInstancingEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_INSTANCE_INFO_GPU_INSTANCING_EXT));
    const uint32_t count = instances ? instances->instanceTransforms_count : 1;
    if (instances && count && !instances->instanceTransforms_values) {
      throw invalid;
    }
    if (uint64_t(count) * mesh->second->surfaces.size() + m_draws.size() > 65536) {
      throw invalid;
    }
    if (!m_default) {
      m_default = std::make_shared<Material>();
      m_default->texture = texturePath(nullptr);
    }
    std::vector<Draw> pending;
    for (uint32_t i = 0; i < count; ++i) {
      auto instance = instances ? multiply(model, transform(instances->instanceTransforms_values[i])) : model;
      finite(instance.data(), 16);
      for (const auto &surface : mesh->second->surfaces) {
        Draw draw;
        draw.surface = surface;
        draw.mesh = mesh->second;
        draw.surfaceIndex = uint32_t(&surface - mesh->second->surfaces.data());
        draw.instanceIndex = i;
        if (identity) {
          draw.instanceId = identity->instanceId;
          draw.classification = identity->classification;
          draw.rasterVisible = identity->rasterVisible != 0;
        }
        draw.model = instance;
        draw.bones = bones;
        if (rasterState) {
          draw.depth = rasterState->depthTest != 0;
          draw.depthWriteOverride = true;
          draw.depthWrite = rasterState->depthWrite != 0;
          draw.depthCompare = rasterState->depthCompare;
        }
        draw.depthBias = (info.categoryFlags & (REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_DYNAMIC |
                                                REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_STATIC |
                                                REMIXAPI_INSTANCE_CATEGORY_BIT_DECAL_SINGLE_OFFSET)) != 0;
        draw.material = m_default;
        if (surface->material) {
          auto it = m_materials.find(key(surface->material));
          if (it == m_materials.end()) {
            throw invalid;
          }
          draw.material = it->second;
        }
        if (surface->bonesPerVertex) {
          if (!bones || bones->empty()) {
            throw invalid;
          }
          for (auto index : surface->bones) {
            if (index >= bones->size()) {
              throw invalid;
            }
          }
        }
        draw.blend = state;
        if (rasterState && rasterState->disableColorWrites)
          draw.blend.mask = 0;
        draw.blend.enabled |= draw.material->translucent;
        draw.doubleSided = info.doubleSided != 0;
        draw.ignoreAlpha = (info.categoryFlags & REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL) != 0;
        if (draw.ignoreAlpha) {
          draw.blend.enabled = false;
        }
        draw.alphaOverride = draw.material->drawAlpha && alpha;
        if (draw.alphaOverride && alpha->alphaTestEnabled) {
          draw.compare = alpha->alphaTestCompareOp;
          draw.reference = float(alpha->alphaTestReferenceValue) / 255;
        }
        if (info.categoryFlags & REMIXAPI_INSTANCE_CATEGORY_BIT_VIEW_MODEL) {
          draw.camera = REMIXAPI_CAMERA_TYPE_VIEW_MODEL;
        } else if (info.categoryFlags & REMIXAPI_INSTANCE_CATEGORY_BIT_SKY) {
          draw.camera = REMIXAPI_CAMERA_TYPE_SKY;
          draw.depth = false;
        }
        pending.push_back(std::move(draw));
      }
    }
    if (m_draws.capacity() < m_draws.size() + pending.size())
      m_draws.reserve(std::max(m_draws.size() + pending.size(), m_draws.capacity() * 2));
    if (traceIdentity && !pending.empty())
      m_frameIds.insert(identity->instanceId);
    m_draws.insert(m_draws.end(), std::make_move_iterator(pending.begin()),
                   std::make_move_iterator(pending.end()));
  }
} // namespace remix
