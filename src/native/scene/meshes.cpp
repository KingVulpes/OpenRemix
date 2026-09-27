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

  void NativeResources::createMesh(const remixapi_MeshInfo &info, remixapi_MeshHandle &handle) {
    if (!info.hash || !info.surfaces_count || info.surfaces_count > 4096 || !info.surfaces_values) {
      throw invalid;
    }
    const auto *update = static_cast<const remixapi_MeshInfoUpdateEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MESH_UPDATE_EXT));
    if (update && update->enabled > 1) throw invalid;
    const auto *motion = static_cast<const remixapi_MeshInfoMotionEXT *>(
        extension(info.pNext, REMIXAPI_STRUCT_TYPE_MESH_MOTION_EXT));
    if (motion && (motion->enabled > 1 || (motion->enabled && (!update || !update->enabled)))) throw invalid;
    auto mesh = std::make_shared<Mesh>();
    mesh->motionCorrespondence = motion && motion->enabled;
    uint64_t bytes = 0;
    for (uint32_t i = 0; i < info.surfaces_count; ++i) {
      const auto &source = info.surfaces_values[i];
      if (!source.vertices_values || !source.vertices_count || source.vertices_count > 8 * 1024 * 1024 ||
          source.indices_count > 24 * 1024 * 1024 || (source.indices_count && !source.indices_values) ||
          (source.indices_count ? source.indices_count : source.vertices_count) % 3) {
        throw invalid;
      }
      if (source.material && m_materials.find(key(source.material)) == m_materials.end()) {
        throw invalid;
      }
      bytes += source.vertices_count * sizeof(Vertex) +
               (source.indices_count ? source.indices_count : source.vertices_count) * 4;
      if (bytes > 512u * 1024 * 1024) {
        throw invalid;
      }
      auto surface = std::make_shared<Surface>();
      surface->material = source.material;
      surface->vertices.resize(size_t(source.vertices_count));
      std::array<float, 3> low{INFINITY, INFINITY, INFINITY}, high{-INFINITY, -INFINITY, -INFINITY};
      for (size_t v = 0; v < surface->vertices.size(); ++v) {
        const auto &src = source.vertices_values[v];
        finite(src.position, 3);
        finite(src.texcoord, 2);
        auto &dst = surface->vertices[v];
        std::copy_n(src.position, 3, dst.position);
        std::copy_n(src.texcoord, 2, dst.uv);
        // Legacy callers may omit normals. Invalid/zero normals retain flat
        // shading; a valid authored normal must survive scene ownership.
        if (std::isfinite(src.normal[0]) && std::isfinite(src.normal[1]) && std::isfinite(src.normal[2])) {
          const float length = std::hypot(src.normal[0], src.normal[1], src.normal[2]);
          if (length > 1e-12f)
            for (int c = 0; c < 3; ++c) dst.normal[c] = src.normal[c] / length;
        }
        rgba(src.color, dst.color);
        for (int c = 0; c < 3; ++c) {
          low[c] = std::min(low[c], src.position[c]);
          high[c] = std::max(high[c], src.position[c]);
        }
      }
      for (int c = 0; c < 3; ++c) {
        surface->center[c] = low[c] * .5f + high[c] * .5f;
      }
      if (source.indices_count) {
        surface->indices.assign(source.indices_values, source.indices_values + source.indices_count);
        for (auto index : surface->indices) {
          if (index >= source.vertices_count) {
            throw invalid;
          }
        }
      } else {
        surface->indices.resize(size_t(source.vertices_count));
        std::iota(surface->indices.begin(), surface->indices.end(), 0u);
      }
      if (source.skinning_hasvalue) {
        const auto &skin = source.skinning_value;
        const uint64_t count = source.vertices_count * skin.bonesPerVertex;
        if (!skin.bonesPerVertex || skin.bonesPerVertex > 256 || count != skin.blendWeights_count ||
            count != skin.blendIndices_count || !skin.blendWeights_values || !skin.blendIndices_values) {
          throw invalid;
        }
        bytes += count * 8;
        if (bytes > 512u * 1024 * 1024) {
          throw invalid;
        }
        surface->bonesPerVertex = skin.bonesPerVertex;
        surface->weights.assign(skin.blendWeights_values, skin.blendWeights_values + count);
        surface->bones.assign(skin.blendIndices_values, skin.blendIndices_values + count);
        for (size_t n = 0; n < count; ++n) {
          if (surface->bones[n] >= 256 || !std::isfinite(surface->weights[n]) || surface->weights[n] < 0) {
            throw invalid;
          }
        }
      }
      mesh->surfaces.push_back(std::move(surface));
    }
    auto previous = m_meshes.find(info.hash);
    if (update && update->enabled && std::none_of(mesh->surfaces.begin(), mesh->surfaces.end(),
                                                 [](const auto &s) { return s->bonesPerVertex != 0; })) {
      bool compatible = previous != m_meshes.end() && previous->second->updateStream &&
                        previous->second->motionCorrespondence == mesh->motionCorrespondence &&
                        previous->second->surfaces.size() == mesh->surfaces.size();
      if (compatible) for (size_t i = 0; i < mesh->surfaces.size(); ++i) {
        const auto &old = *previous->second->surfaces[i], &next = *mesh->surfaces[i];
        if (old.vertices.size() != next.vertices.size() || old.indices != next.indices) { compatible = false; break; }
      }
      mesh->updateStream = compatible ? previous->second->updateStream : std::make_shared<MeshUpdateStream>();
    }
    auto equal = [&](const Mesh &a, const Mesh &b) {
      if (a.updateStream != b.updateStream || a.motionCorrespondence != b.motionCorrespondence) return false;
      if (a.surfaces.size() != b.surfaces.size())
        return false;
      for (size_t i = 0; i < a.surfaces.size(); ++i) {
        const auto &x = *a.surfaces[i], &y = *b.surfaces[i];
        if (x.material != y.material || x.vertices.size() != y.vertices.size() || x.indices != y.indices ||
            x.weights != y.weights || x.bones != y.bones || x.bonesPerVertex != y.bonesPerVertex ||
            std::memcmp(x.vertices.data(), y.vertices.data(), x.vertices.size() * sizeof(Vertex)))
          return false;
      }
      return true;
    };
    if (previous == m_meshes.end() || !equal(*previous->second, *mesh)) {
      if (previous != m_meshes.end())
        previous->second->retired = true;
      m_meshes[info.hash] = std::move(mesh);
      ++m_resourceGeneration;
    }
    handle = reinterpret_cast<remixapi_MeshHandle>(uintptr_t(info.hash));
  }

  void NativeResources::destroyMesh(remixapi_MeshHandle handle) {
    if (auto it = m_meshes.find(key(handle)); it != m_meshes.end()) {
      it->second->retired = true;
      m_meshes.erase(it);
    }
  }
} // namespace remix
