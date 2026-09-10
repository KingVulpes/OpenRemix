#pragma once
#include "scene/temporal.h"
#include "scene/types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <remix/remix_c.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace remix {

  // CPU state is serialized by the API mutex. Only the startup thread touches GPU state.
  class NativeResources {
  public:
    NativeResources();
    ~NativeResources() = default;
    NativeResources(const NativeResources &) = delete;
    NativeResources &operator=(const NativeResources &) = delete;
    void reset();
    void createMaterial(const remixapi_MaterialInfo &info, remixapi_MaterialHandle &handle);
    void destroyMaterial(remixapi_MaterialHandle handle);
    void createMesh(const remixapi_MeshInfo &info, remixapi_MeshHandle &handle);
    void destroyMesh(remixapi_MeshHandle handle);
    void createTexture(const remixapi_TextureInfo &info, remixapi_TextureHandle &handle);
    void destroyTexture(remixapi_TextureHandle handle);
    void setupCamera(const remixapi_CameraInfo &info);
    void drawInstance(const remixapi_InstanceInfo &info);
    void registerUiTexture(uint64_t id, uint32_t width, uint32_t height, remixapi_Format format,
                           const void *pixels, uint64_t size);
    void freeUiTexture(uint64_t id);
    void submitUi(const remixapi_UIDrawList *info);
    void overlay(const void *pixels, uint32_t width, uint32_t height, remixapi_Format format, float opacity);
    void setTint(float r, float g, float b, float a);
    void finishFrame();
    size_t pendingDrawCount() const { return m_draws.size(); }

    const std::vector<scene::Draw> &draws() const { return m_draws; }
    const scene::Matrix &camera(uint32_t type = 0) const { return m_cameras[type]; }
    const scene::Matrix &view(uint32_t type = 0) const { return m_views[type]; }
    bool cameraValid() const { return m_cameraValid[0]; }
    scene::TemporalEpoch temporalEpoch() const { return m_temporalEpoch; }
    void createLight(const remixapi_LightInfo &info, remixapi_LightHandle &handle);
    void updateLight(remixapi_LightHandle handle, const remixapi_LightInfo &info);
    void destroyLight(remixapi_LightHandle handle);
    void drawLight(remixapi_LightHandle handle);
    std::shared_ptr<scene::Material> defaultMaterial() {
      if (!m_default) {
        m_default = std::make_shared<scene::Material>();
        m_default->texture = texturePath(nullptr);
      }
      return m_default;
    }
    const scene::DistantLight &light() const { return m_light; }

  protected:
    std::shared_ptr<scene::Texture> texturePath(remixapi_Path path);
    std::shared_ptr<scene::Texture> m_white;
    std::shared_ptr<scene::Material> m_default;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Material>> m_materials;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Mesh>> m_meshes;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Texture>> m_textures, m_uiTextures;
    std::unordered_map<std::filesystem::path, std::weak_ptr<scene::Texture>> m_paths;
    std::array<scene::Matrix, 3> m_cameras, m_views;
    std::unordered_map<uint64_t, scene::DistantLight> m_lights;
    scene::DistantLight m_light;
    std::array<bool, 3> m_cameraValid = {};
    scene::TemporalEpoch m_temporalEpoch;
    std::vector<scene::Draw> m_draws, m_uiDraws, m_overlay;
    std::unordered_set<uint64_t> m_frameIds;
    float m_tint[4] = {};
  };
} // namespace remix
