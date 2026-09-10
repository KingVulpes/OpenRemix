#include "pathtrace/gpu.h"

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  VkBuildAccelerationStructureFlagsKHR PathTracer::Gpu::flags(bool deform) const {
    return VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
           (deform ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                   : VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
  }

  PathTracer::Gpu::MeshView &PathTracer::Gpu::mesh(VkCommandBuffer command, const Draw &draw) {
    const bool deform = draw.bones && std::any_of(draw.mesh->surfaces.begin(), draw.mesh->surfaces.end(),
                                                  [](const auto &s) { return s->bonesPerVertex != 0; });
    // Each independently animated skin owns its update-capable BLAS. Rigid parts share canonical BLASes.
    auto key =
        std::make_tuple(draw.mesh.get(), deform ? draw.instanceId : 0, deform ? draw.instanceIndex : 0);
    auto &view = meshes[key];
    view.lastUse = sequence;
    if (!view.owner.expired() && (!deform || view.bones == *draw.bones))
      return view;
    // A replaced CPU mesh may reuse an address. Keep its geometry alive through
    // the canonical lookup, but never mistake its view for the new mesh.
    auto previousGeometry = view.owner.expired() ? std::move(view.geometry) : nullptr;
    view.owner = draw.mesh;
    if (deform)
      view.bones = *draw.bones;
    std::string content;
    auto append = [&](const void *p, size_t size) { content.append(static_cast<const char *>(p), size); };
    std::vector<std::vector<TraceVertex>> shading;
    std::vector<VkDeviceSize> vertexOffsets, indexOffsets;
    std::vector<uint32_t> counts, vertexCounts;
    float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const auto &surface : draw.mesh->surfaces) {
      uint32_t count[2] = {uint32_t(surface->vertices.size()), uint32_t(surface->indices.size())};
      append(count, sizeof(count));
      vertexOffsets.push_back(content.size());
      auto &vertices = shading.emplace_back();
      for (size_t i = 0; i < surface->vertices.size(); ++i) {
        auto vertex = surface->vertices[i];
        if (surface->bonesPerVertex) {
          const auto original = vertex;
          std::fill_n(vertex.position, 3, 0.f);
          for (uint32_t b = 0; b < surface->bonesPerVertex; ++b) {
            size_t index = i * surface->bonesPerVertex + b;
            const auto &matrix = (*draw.bones)[surface->bones[index]];
            for (int r = 0; r < 3; ++r)
              vertex.position[r] += surface->weights[index] *
                                    (matrix[r] * original.position[0] + matrix[4 + r] * original.position[1] +
                                     matrix[8 + r] * original.position[2] + matrix[12 + r]);
          }
        }
        append(vertex.position, 12);
        for (int r = 0; r < 3; ++r) {
          low[r] = std::min(low[r], vertex.position[r]);
          high[r] = std::max(high[r], vertex.position[r]);
        }
        TraceVertex out;
        std::copy_n(vertex.position, 3, out.position);
        std::copy_n(vertex.uv, 2, out.uv);
        std::copy_n(vertex.color, 4, out.color);
        vertices.push_back(out);
      }
      indexOffsets.push_back(content.size());
      append(surface->indices.data(), surface->indices.size() * 4);
      counts.push_back(count[1] / 3);
      vertexCounts.push_back(count[0]);
    }
    if (!deform) {
      auto found = geometryCache.find(content);
      if (found != geometryCache.end())
        view.geometry = found->second.lock();
    }
    if (!view.geometry || deform) {
      bool update = view.geometry != nullptr;
      if (!view.geometry)
        view.geometry = std::make_shared<Geometry>();
      auto &g = *view.geometry;
      g.deform = deform;
      g.update = update;
      g.pending = true;
      g.counts = counts;
      g.indexOffsets = indexOffsets;
      g.fingerprint = scene::hashBytes(content.data(), content.size());
      ++g.revision;
      g.diagonal = std::hypot(high[0] - low[0], high[1] - low[1], high[2] - low[2]);
      g.input =
          upload(command, content.data(), content.size(), inputUsage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      g.geometries.clear();
      g.ranges.clear();
      for (size_t i = 0; i < counts.size(); ++i) {
        VkAccelerationStructureGeometryKHR geometry = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR};
        geometry.geometry.triangles = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData = {.deviceAddress = g.input->address + vertexOffsets[i]},
            .vertexStride = 12,
            .maxVertex = vertexCounts[i] - 1,
            .indexType = VK_INDEX_TYPE_UINT32,
            .indexData = {.deviceAddress = g.input->address + indexOffsets[i]}};
        g.geometries.push_back(geometry);
        g.ranges.push_back({.primitiveCount = counts[i]});
      }
      VkAccelerationStructureBuildGeometryInfoKHR build = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
          .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
          .flags = flags(deform),
          .geometryCount = uint32_t(g.geometries.size()),
          .pGeometries = g.geometries.data()};
      buildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, counts.data(), &g.sizes);
      if (!g.accel)
        g.accel =
            acceleration(g.sizes.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
      if (!deform)
        geometryCache[content] = view.geometry;
    }
    view.vertices.clear();
    view.fingerprint = view.geometry->fingerprint;
    for (const auto &vertices : shading) {
      view.fingerprint =
          scene::hashBytes(vertices.data(), vertices.size() * sizeof(TraceVertex), view.fingerprint);
      view.vertices.push_back(upload(command, vertices.data(), vertices.size() * sizeof(TraceVertex),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    }
    return view;
  }

} // namespace remix
