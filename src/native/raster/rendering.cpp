#include "raster/gpu.h"

namespace remix {
  using namespace scene;
  using namespace raster::detail;

  void RasterRenderer::prepare(VkCommandBuffer command, bool tracing) {
    auto &g = *m_gpu;
    g.commands.clear();
    for (auto &buffer : g.uploads)
      g.recycle(std::move(buffer));
    g.uploads.clear();
    g.recycle(std::move(g.skin));
    auto &retired = g.frames[g.frameIndex];
    for (auto it = g.materials.begin(); it != g.materials.end();) {
      if (it->second->owner.expired()) {
        retired.retiredMaterials.push_back(std::move(it->second));
        it = g.materials.erase(it);
      } else
        ++it;
    }
    for (auto it = g.textures.begin(); it != g.textures.end();) {
      if (it->second.owner.expired()) {
        retired.retiredImages.push_back(std::move(it->second.data));
        it = g.textures.erase(it);
      } else
        ++it;
    }
    for (auto it = g.meshes.begin(); it != g.meshes.end();) {
      if (it->second.owner.expired()) {
        retired.retiredBuffers.push_back(std::move(it->second.data));
        it = g.meshes.erase(it);
      } else
        ++it;
    }
    prune(m_paths, [](const auto &entry) { return entry.second.expired(); });
    std::vector<Draw> ordered;
    for (const auto &draw : m_draws) {
      if (draw.rasterVisible && (!tracing || !draw.traced())) {
        auto copy = draw;
        // Traced barycentric depth and raster interpolation can differ by ULPs.
        // Bias coplanar EQUAL feedback toward the camera while preserving occlusion.
        if (tracing && copy.depthCompare == 2) {
          copy.depthCompare = 3;
          copy.depthBias = true;
        }
        ordered.push_back(std::move(copy));
      }
    }
    auto layer = [](const Draw &d) {
      return d.ui                                          ? 3
             : d.camera == REMIXAPI_CAMERA_TYPE_SKY        ? 0
             : d.camera == REMIXAPI_CAMERA_TYPE_VIEW_MODEL ? 2
                                                           : 1;
    };
    auto distance = [&](const Draw &d) {
      const auto mvp = multiply(m_cameras[d.camera], d.model);
      const auto &c = d.surface->center;
      const float w = mvp[3] * c[0] + mvp[7] * c[1] + mvp[11] * c[2] + mvp[15];
      const float z = mvp[2] * c[0] + mvp[6] * c[1] + mvp[10] * c[2] + mvp[14];
      const float depth = z / w;
      return std::isfinite(depth) ? depth : 0.0f;
    };
    std::stable_sort(ordered.begin(), ordered.end(), [&](const Draw &a, const Draw &b) {
      if (layer(a) != layer(b)) {
        return layer(a) < layer(b);
      }
      if (a.blend.enabled != b.blend.enabled) {
        return !a.blend.enabled;
      }
      return a.blend.enabled && distance(a) > distance(b);
    });
    if (m_tint[3] > 0) {
      Draw tint;
      tint.ui = true;
      tint.depth = false;
      tint.doubleSided = true;
      tint.blend.enabled = true;
      tint.canvasWidth = tint.canvasHeight = 1;
      tint.material = std::make_shared<Material>();
      tint.material->texture = texturePath(nullptr);
      std::copy_n(m_tint, 4, tint.material->tint);
      tint.surface = std::make_shared<Surface>();
      tint.surface->vertices = {{{0, 0, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{1, 0, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{1, 1, 0}, {0, 0}, {1, 1, 1, 1}},
                                {{0, 1, 0}, {0, 0}, {1, 1, 1, 1}}};
      tint.surface->indices = {0, 1, 2, 0, 2, 3};
      ordered.push_back(std::move(tint));
    }
    ordered.insert(ordered.end(), m_overlay.begin(), m_overlay.end());
    ordered.insert(ordered.end(), m_uiDraws.begin(), m_uiDraws.end());
    std::vector<Vertex> skinned;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - g.epoch).count();
    for (const auto &draw : ordered) {
      const auto &mat = *draw.material;
      auto &mesh = g.mesh(draw.surface, command);
      Gpu::Command out = {};
      out.set = g.material(draw.material, command);
      out.pipeline = g.pipeline(draw);
      out.vertices = out.indices = mesh.data->buffer;
      out.indexOffset = mesh.indexOffset;
      out.firstIndex = draw.firstIndex;
      out.count = draw.indexCount ? draw.indexCount : uint32_t(draw.surface->indices.size());
      out.baseVertex = draw.vertexOffset;
      out.layer = layer(draw);
      if (draw.surface->bonesPerVertex) {
        out.vertices = VK_NULL_HANDLE;
        out.vertexOffset = skinned.size() * sizeof(Vertex);
        if (skinned.size() + draw.surface->vertices.size() > 8 * 1024 * 1024) {
          throw VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        for (size_t v = 0; v < draw.surface->vertices.size(); ++v) {
          Vertex vertex = draw.surface->vertices[v];
          const auto original = vertex;
          std::fill_n(vertex.position, 3, 0.0f);
          for (uint32_t b = 0; b < draw.surface->bonesPerVertex; ++b) {
            const size_t index = v * draw.surface->bonesPerVertex + b;
            const auto &m = (*draw.bones)[draw.surface->bones[index]];
            for (int r = 0; r < 3; ++r) {
              vertex.position[r] += draw.surface->weights[index] *
                                    (m[r] * original.position[0] + m[4 + r] * original.position[1] +
                                     m[8 + r] * original.position[2] + m[12 + r]);
            }
          }
          skinned.push_back(vertex);
        }
      }
      out.push.mvp = multiply(m_cameras[draw.camera], draw.model);
      if (draw.ui) {
        out.push.mvp = identity();
        out.push.mvp[0] = 2.0f / draw.canvasWidth;
        out.push.mvp[5] = 2.0f / draw.canvasHeight;
        out.push.mvp[12] = out.push.mvp[13] = -1;
      } else if (!m_cameraValid[draw.camera] && draw.camera != REMIXAPI_CAMERA_TYPE_WORLD) {
        out.push.mvp = multiply(m_cameras[REMIXAPI_CAMERA_TYPE_WORLD], draw.model);
      }
      std::copy_n(mat.tint, 4, out.push.tint);
      const uint32_t frame = uint32_t(std::fmod(seconds * mat.fps, uint32_t(mat.rows) * mat.columns));
      out.push.uv[0] = 1.0f / mat.columns;
      out.push.uv[1] = 1.0f / mat.rows;
      out.push.uv[2] = float(frame % mat.columns) / mat.columns;
      out.push.uv[3] = float(frame / mat.columns) / mat.rows;
      const auto &b = draw.blend;
      out.push.state[0] =
          b.colorOp | b.color1 << 2 | b.color2 << 4 | b.alphaOpTexture << 6 | b.alpha1 << 8 | b.alpha2 << 10;
      out.push.state[1] = draw.ignoreAlpha ? 7 : draw.alphaOverride ? draw.compare : mat.compare;
      const float reference = draw.alphaOverride ? draw.reference : mat.reference;
      std::memcpy(&out.push.state[2], &reference, sizeof(reference));
      out.push.state[3] =
          uint32_t(draw.ignoreAlpha) |
          (g.encodeOutput && !(g.skyPass && draw.camera == REMIXAPI_CAMERA_TYPE_SKY) ? 2u : 0u);
      out.push.factor[0] = linear(float((b.factor >> 16) & 255) / 255);
      out.push.factor[1] = linear(float((b.factor >> 8) & 255) / 255);
      out.push.factor[2] = linear(float(b.factor & 255) / 255);
      out.push.factor[3] = float(b.factor >> 24) / 255;
      g.commands.push_back(out);
    }
    if (!skinned.empty()) {
      g.skin = g.geometryBuffer(skinned.data(), skinned.size() * sizeof(Vertex),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, command);
      for (auto &draw : g.commands) {
        if (!draw.vertices) {
          draw.vertices = g.skin->buffer;
        }
      }
    }
  }

  void RasterRenderer::record(VkCommandBuffer command, bool skyOnly, bool skipSky, const float *skyJitter, VkExtent2D skyExtent) {
    auto &g = *m_gpu;
    const auto extent = skyOnly && skyExtent.width ? skyExtent : g.extent;
    const VkViewport viewport = {0, 0, float(extent.width), float(extent.height), 0, 1};
    const VkRect2D scissor = {{0, 0}, extent};
    vkCmdSetViewport(command, 0, 1, &viewport);
    vkCmdSetScissor(command, 0, 1, &scissor);
    int previousLayer = -1;
    for (const auto &draw : g.commands) {
      if ((skyOnly && draw.layer != 0) || (skipSky && draw.layer == 0))
        continue;
      if (draw.layer != previousLayer && draw.layer >= 2) {
        VkClearAttachment clear = {};
        clear.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        clear.clearValue.depthStencil.depth = 1;
        const VkClearRect rect = {scissor, 0, 1};
        vkCmdClearAttachments(command, 1, &clear, 1, &rect);
      }
      previousLayer = draw.layer;
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_GRAPHICS, draw.pipeline);
      vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &draw.set, 0,
                              nullptr);
      vkCmdBindVertexBuffers(command, 0, 1, &draw.vertices, &draw.vertexOffset);
      vkCmdBindIndexBuffer(command, draw.indices, draw.indexOffset, VK_INDEX_TYPE_UINT32);
      vkCmdSetBlendConstants(command, draw.push.factor);
      auto push = draw.push;
      if (skyJitter && draw.layer == 0) {
        // Match the primary ray sample; raster overlays keep their center sample.
        for (int column = 0; column < 4; ++column) {
          push.mvp[column * 4] -= 2 * skyJitter[0] / extent.width * push.mvp[column * 4 + 3];
          push.mvp[column * 4 + 1] -= 2 * skyJitter[1] / extent.height * push.mvp[column * 4 + 3];
        }
      }
      vkCmdPushConstants(command, g.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(Push), &push);
      vkCmdDrawIndexed(command, draw.count, 1, draw.firstIndex, draw.baseVertex, 0);
    }
  }

} // namespace remix
