#include "pathtrace/gpu.h"
#include <string_view>

namespace remix {
  using namespace scene;
  using namespace trace::detail;

  namespace {
    uint64_t bufferFingerprint(const void *data, size_t size, uint64_t seed = 1469598103934665603ull) {
      // These fingerprints only invalidate history inside this renderer run.
      // Bulk hashing avoids a serial byte-at-a-time multiply over every changed
      // vertex. Geometry cache reuse still compares the complete content string.
      const uint64_t digest = std::hash<std::string_view>{}({static_cast<const char *>(data), size});
      return scene::hashBytes(&digest, sizeof(digest), seed);
    }
  } // namespace

  VkBuildAccelerationStructureFlagsKHR PathTracer::Gpu::flags(bool deform) const {
    return VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
           (deform ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR
                   : VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
  }

  PathTracer::Gpu::MeshView &PathTracer::Gpu::mesh(VkCommandBuffer command, const Draw &draw,
      const std::vector<std::shared_ptr<const scene::OpacityPartition>> &partitions) {
    ScopedMilliseconds time{performance.geometryMilliseconds};
    std::string opaque;
    for (const auto &partition : partitions) opaque.append(partition->key);
    const bool skinned = draw.bones && std::any_of(draw.mesh->surfaces.begin(), draw.mesh->surfaces.end(),
                                                  [](const auto &s) { return s->bonesPerVertex != 0; });
    const bool streaming = bool(draw.mesh->updateStream);
    const bool deform = skinned || streaming;
    // Each independently animated skin owns its BLAS. Opt-in CPU replacements
    // may transfer compatible storage only from a view unused in this frame;
    // a skinned lineage transfers per instance.
    auto key =
        std::make_tuple(draw.mesh.get(), skinned ? draw.instanceId : 0, skinned ? draw.instanceIndex : 0, opaque);
    auto &view = meshes[key];
    if (streaming && view.owner.lock() != draw.mesh) {
      const UpdateViewKey lineage{draw.mesh->updateStream.get(), std::get<1>(key), std::get<2>(key), opaque};
      auto found = updateViews.find(lineage);
      if (found != updateViews.end() && found->second != key) {
        auto old = meshes.find(found->second);
        if (old != meshes.end() && old->second.updateStream == draw.mesh->updateStream &&
            old->second.lastUse != sequence) {
          if (view.geometry) retiredGeometry.push_back(std::move(view.geometry));
          for (auto &vertices : view.vertices) retiredBuffers.push_back(std::move(vertices));
          view = std::move(old->second); meshes.erase(old);
        }
      }
      updateViews[lineage] = key;
    }
    view.lastUse = sequence;
    if (view.owner.lock() == draw.mesh && (!skinned || view.bones == *draw.bones))
      return view;
    ++performance.geometryConversions;
    // A replaced CPU mesh may reuse an address. Keep its geometry alive through
    // the canonical lookup, but never mistake its view for the new mesh.
    const bool transfer = streaming && view.updateStream == draw.mesh->updateStream;
    auto previousGeometry = !transfer && view.owner.lock() != draw.mesh ? std::move(view.geometry) : nullptr;
    view.owner = draw.mesh;
    view.updateStream = draw.mesh->updateStream;
    if (skinned)
      view.bones = *draw.bones;
    struct TrimScratch {
      GeometryScratch &scratch;
      ~TrimScratch() { if (scratch.bytes() > 64*1024*1024) scratch = {}; }
    } trim{conversion};
    auto &content = conversion.content;
    content.clear();
    size_t contentBytes = opaque.size() + sizeof(uint32_t);
    for (const auto &surface : draw.mesh->surfaces)
      contentBytes += 12 + surface->vertices.size() * 12 + surface->indices.size() * 4;
    content.reserve(contentBytes);
    auto append = [&](const void *p, size_t size) { content.append(static_cast<const char *>(p), size); };
    auto &shading = conversion.shading;
    auto &vertexOffsets = conversion.vertexOffsets, &indexOffsets = conversion.indexOffsets;
    auto &counts = conversion.counts, &vertexCounts = conversion.vertexCounts;
    vertexOffsets.clear(); indexOffsets.clear(); counts.clear(); vertexCounts.clear();
    const auto surfaceCount = draw.mesh->surfaces.size();
    const auto surfaceHeader = uint32_t(surfaceCount);
    append(&surfaceHeader, sizeof(surfaceHeader));
    shading.resize(surfaceCount);
    size_t surfaceIndex = 0;
    std::vector<GeometryGroup> groups;
    vertexOffsets.reserve(surfaceCount); indexOffsets.reserve(surfaceCount);
    counts.reserve(surfaceCount); vertexCounts.reserve(surfaceCount);
    float low[3] = {INFINITY, INFINITY, INFINITY}, high[3] = {-INFINITY, -INFINITY, -INFINITY};
    for (const auto &surface : draw.mesh->surfaces) {
      uint32_t count[3] = {uint32_t(surface->vertices.size()), uint32_t(surface->indices.size()),
                           partitions[surfaceIndex]->key[0] == 2 ? 2u : 1u};
      append(count, sizeof(count));
      vertexOffsets.push_back(content.size());
      auto &vertices = shading[surfaceIndex];
      vertices.resize(surface->vertices.size());
      for (size_t i = 0; i < surface->vertices.size(); ++i) {
        auto vertex = surface->vertices[i];
        if (surface->bonesPerVertex) {
          const auto original = vertex;
          std::fill_n(vertex.position, 3, 0.f);
          std::fill_n(vertex.normal, 3, 0.f);
          for (uint32_t b = 0; b < surface->bonesPerVertex; ++b) {
            size_t index = i * surface->bonesPerVertex + b;
            const auto &matrix = (*draw.bones)[surface->bones[index]];
            for (int r = 0; r < 3; ++r)
              vertex.position[r] += surface->weights[index] *
                                    (matrix[r] * original.position[0] + matrix[4 + r] * original.position[1] +
                                     matrix[8 + r] * original.position[2] + matrix[12 + r]);
            for (int r = 0; r < 3; ++r)
              vertex.normal[r] += surface->weights[index] *
                  (matrix[r] * original.normal[0] + matrix[4 + r] * original.normal[1] +
                   matrix[8 + r] * original.normal[2]);
          }
          const float length = std::hypot(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
          if (std::isfinite(length) && length > 1e-12f)
            for (float &component : vertex.normal) component /= length;
          else
            std::fill_n(vertex.normal, 3, 0.f);
        }
        append(vertex.position, 12);
        for (int r = 0; r < 3; ++r) {
          low[r] = std::min(low[r], vertex.position[r]);
          high[r] = std::max(high[r], vertex.position[r]);
        }
        auto &out = vertices[i];
        std::copy_n(vertex.position, 3, out.position);
        std::copy_n(vertex.uv, 2, out.uv);
        std::copy_n(vertex.color, 4, out.color);
        // Reuse the existing shading-vertex padding, preserving its 48-byte
        // stride and the position-only BLAS input size.
        out.position[3] = vertex.normal[0];
        out.uv[2] = vertex.normal[1]; out.uv[3] = vertex.normal[2];
      }
      indexOffsets.push_back(content.size());
      append(surface->indices.data(), surface->indices.size() * 4);
      const auto &partition = *partitions[surfaceIndex];
      if (partition.key[0] != 2) {
        groups.push_back({uint32_t(surfaceIndex), count[1] / 3, indexOffsets.back(), 0, partition.key[0] == 1});
        counts.push_back(count[1] / 3);
      } else {
        // AS/shading indices share the partition order. Raster indices, vertex
        // correspondence and emitter/debug identities retain their original order.
        for (bool solid : {true, false}) {
          GeometryGroup group{uint32_t(surfaceIndex), 0, content.size(), 0, solid};
          for (uint32_t triangle = 0; triangle < partition.triangles; ++triangle)
            if (partition.opaque(triangle) == solid) {
              append(surface->indices.data() + triangle * 3, 12); ++group.count;
            }
          group.triangleMap = content.size();
          for (uint32_t triangle = 0; triangle < partition.triangles; ++triangle)
            if (partition.opaque(triangle) == solid) append(&triangle, sizeof(triangle));
          counts.push_back(group.count); groups.push_back(group);
        }
      }
      vertexCounts.push_back(count[0]);
      ++surfaceIndex;
    }
    // Opacity is part of BLAS identity, including independently skinned and
    // streamed variants. Append after geometry data to preserve input alignment.
    content.append(opaque);
    if (!deform) {
      auto found = geometryCache.find(content);
      if (found != geometryCache.end())
        view.geometry = found->second.lock();
      if (view.geometry) ++performance.geometryCacheHits;
    }
    if (!view.geometry || deform) {
      bool update = view.geometry != nullptr;
      if (!view.geometry)
        view.geometry = std::make_shared<Geometry>();
      auto &g = *view.geometry;
      g.deform = deform;
      // Refit only while the build remains spatially similar, and periodically
      // rebuild to bound degradation from many small deformations. Skins keep
      // refitting as without a lineage: their bones move the whole bound.
      if (update && streaming && !skinned) {
        const float limit = std::max(g.diagonal * .25f, .001f);
        if (g.refits >= 60) update = false;
        for (unsigned c = 0; c < 3; ++c)
          if (std::abs(low[c] - g.buildLow[c]) > limit || std::abs(high[c] - g.buildHigh[c]) > limit) update = false;
      }
      g.update = update;
      if (update) ++g.refits;
      else {
        g.refits = 0;
        std::copy_n(low, 3, g.buildLow.begin()); std::copy_n(high, 3, g.buildHigh.begin());
      }
      g.pending = true;
      g.counts = counts;
      g.indexOffsets = indexOffsets;
      g.groups = groups;
      g.fingerprint = bufferFingerprint(content.data(), content.size());
      ++g.revision;
      g.diagonal = std::hypot(high[0] - low[0], high[1] - low[1], high[2] - low[2]);
      if (g.input)
        retiredBuffers.push_back(std::move(g.input));
      g.input =
          upload(command, content.data(), content.size(), inputUsage | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
      g.geometries.clear();
      g.ranges.clear();
      if (groups.size() > asProperties.maxGeometryCount) throw VK_ERROR_TOO_MANY_OBJECTS;
      for (const auto &group : groups) {
        const auto i = group.surface;
        VkAccelerationStructureGeometryKHR geometry = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
            .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
            .flags = group.opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : VkGeometryFlagsKHR(0)};
        geometry.geometry.triangles = {
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
            .vertexFormat = VK_FORMAT_R32G32B32_SFLOAT,
            .vertexData = {.deviceAddress = g.input->address + vertexOffsets[i]},
            .vertexStride = 12,
            .maxVertex = vertexCounts[i] - 1,
            .indexType = VK_INDEX_TYPE_UINT32,
            .indexData = {.deviceAddress = g.input->address + group.indices}};
        g.geometries.push_back(geometry);
        g.ranges.push_back({.primitiveCount = group.count});
      }
      VkAccelerationStructureBuildGeometryInfoKHR build = {
          .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
          .type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
          .flags = flags(deform),
          .geometryCount = uint32_t(g.geometries.size()),
          .pGeometries = g.geometries.data()};
      if (!g.accel) buildSizes(device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &build, counts.data(), &g.sizes);
      if (!g.accel)
        g.accel =
            acceleration(g.sizes.accelerationStructureSize, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR);
      if (!deform)
        geometryCache[content] = view.geometry;
    }
    for (auto &vertices : view.vertices)
      retiredBuffers.push_back(std::move(vertices));
    view.vertices.clear();
    view.fingerprint = view.geometry->fingerprint;
    for (const auto &vertices : shading) {
      view.fingerprint =
          bufferFingerprint(vertices.data(), vertices.size() * sizeof(TraceVertex), view.fingerprint);
      view.vertices.push_back(upload(command, vertices.data(), vertices.size() * sizeof(TraceVertex),
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT));
    }
    if (previousGeometry)
      retiredGeometry.push_back(std::move(previousGeometry));
    return view;
  }

} // namespace remix
