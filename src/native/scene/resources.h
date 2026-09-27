#pragma once
#include "scene/temporal.h"
#include "scene/types.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <remix/remix_c.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace remix {

  // CPU state is serialized by the API mutex. Only the startup thread touches GPU state.
  // API calls build m_frame; finishFrame() publishes it as m_submitted, which is
  // all that rendering reads, so the next frame can be built while one renders.
  class NativeResources {
  public:
    struct FrameState {
      std::vector<scene::Draw> draws, uiDraws, overlay;
      std::unordered_set<uint64_t> ids, lights;
      std::array<scene::Matrix, 3> cameras = identities(), views = identities(), projections = identities();
      std::array<std::array<float, 4>, 3> reverseDepth{{{0, 0, -1, 1}, {0, 0, -1, 1}, {0, 0, -1, 1}}};
      std::array<bool, 3> cameraValid = {};
      scene::TemporalEpoch temporalEpoch;
      std::array<float, 3> skyRadiance = {};
      scene::DistantLight light;
      scene::DomeLight dome;
      float tint[4] = {};

    private:
      static std::array<scene::Matrix, 3> identities() {
        return {scene::identity(), scene::identity(), scene::identity()};
      }
    };
    NativeResources();
    ~NativeResources() = default;
    NativeResources(const NativeResources &) = delete;
    NativeResources &operator=(const NativeResources &) = delete;
    void reset();
    void clearModMaterials() { m_modMaterials.clear(); }
    void replaceModMaterials(const std::map<uint64_t, remixapi_MaterialHandle> &handles) {
      std::unordered_map<uint64_t, std::shared_ptr<scene::Material>> next;
      for (auto [hash, handle] : handles)
        if (handle)
          next[hash] = m_materials.at(uint64_t(reinterpret_cast<uintptr_t>(handle)));
      m_modMaterials = std::move(next);
    }
    void invalidateTexturePaths() { m_paths.clear(); }
    // Drops expired path-cache entries; runs before a frame is handed to rendering.
    void prunePaths() {
      std::erase_if(m_paths, [](const auto &entry) { return entry.second.expired(); });
    }
    void cacheTexturePath(const std::string &path, const std::shared_ptr<scene::Texture> &texture) {
      m_paths[std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal()] = texture;
    }
    void createMaterial(const remixapi_MaterialInfo &info, remixapi_MaterialHandle &handle);
    void setMaterialSourceHash(remixapi_MaterialHandle handle, uint64_t hash) {
      m_materials.at(uint64_t(reinterpret_cast<uintptr_t>(handle)))->sourceHash = hash;
    }
    void destroyMaterial(remixapi_MaterialHandle handle);
    void createMesh(const remixapi_MeshInfo &info, remixapi_MeshHandle &handle);
    void destroyMesh(remixapi_MeshHandle handle);
    void createTexture(const remixapi_TextureInfo &info, remixapi_TextureHandle &handle);
    void destroyTexture(remixapi_TextureHandle handle);
    void setupCamera(const remixapi_CameraInfo &info);
    void drawInstance(const remixapi_InstanceInfo &info, remixapi_MaterialHandle materialOverride = nullptr);
    void registerUiTexture(uint64_t id, uint32_t width, uint32_t height, remixapi_Format format,
                           const void *pixels, uint64_t size);
    void freeUiTexture(uint64_t id);
    void submitUi(const remixapi_UIDrawList *info);
    void overlay(const void *pixels, uint32_t width, uint32_t height, remixapi_Format format, float opacity);
    void setTint(float r, float g, float b, float a);
    // Publishes this frame's submissions for rendering and starts the next one.
    void finishFrame();
    size_t pendingDrawCount() const { return m_frame.draws.size(); }
    // The frame being built by API calls.
    const std::vector<scene::Draw> &draws() const { return m_frame.draws; }
    const scene::Matrix &camera(uint32_t type = 0) const { return m_frame.cameras[type]; }
    const scene::Matrix &view(uint32_t type = 0) const { return m_frame.views[type]; }
    const scene::Matrix &projection(uint32_t type = 0) const { return m_frame.projections[type]; }
    const std::array<float, 4> &reverseDepth(uint32_t type = 0) const { return m_frame.reverseDepth[type]; }
    bool cameraValid() const { return m_frame.cameraValid[0]; }
    scene::TemporalEpoch temporalEpoch() const { return m_frame.temporalEpoch; }
    const std::array<float, 3> &skyRadiance() const { return m_frame.skyRadiance; }
    // The last published frame: the only frame state rendering may read.
    const FrameState &submitted() const { return m_submitted; }
    void createLight(const remixapi_LightInfo &info, remixapi_LightHandle &handle);
    void updateLight(remixapi_LightHandle handle, const remixapi_LightInfo &info);
    void destroyLight(remixapi_LightHandle handle);
    void drawLight(remixapi_LightHandle handle);
    remixapi_LightHandle overrideLight(remixapi_LightHandle original, uint64_t hash,
                                       const remixapi_LightInfoUSDEXT &overrides, bool transform,
                                       remixapi_Path texture = nullptr);
    std::shared_ptr<scene::Material> defaultMaterial() {
      if (!m_default) {
        m_default = std::make_shared<scene::Material>();
        m_default->texture = texturePath(nullptr);
      }
      return m_default;
    }
    const scene::DomeLight &dome() const { return m_frame.dome; }
    const scene::DistantLight &light() const { return m_frame.light; }
    // Light emitters drawn in the frame being built, or in the submitted frame.
    std::vector<scene::SphereLight> sphereLights() const { return sphereLights(m_frame); }
    std::vector<scene::SphereLight> submittedSphereLights() const { return sphereLights(m_submitted); }
    bool hasSphereLights() const { return !m_submitted.lights.empty(); }

  protected:
    std::vector<scene::SphereLight> sphereLights(const FrameState &frame) const;
    std::shared_ptr<scene::Texture> texturePath(remixapi_Path path);
    std::shared_ptr<scene::Texture> dataTexturePath(remixapi_Path path);
    std::shared_ptr<scene::Texture> m_white;
    std::shared_ptr<scene::Material> m_default;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Material>> m_materials, m_modMaterials;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Mesh>> m_meshes;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Texture>> m_textures, m_uiTextures;
    std::unordered_map<std::filesystem::path, std::weak_ptr<scene::Texture>> m_paths;
    struct LightSource {
      remixapi_StructType type{};
      remixapi_Transform transform{};
      remixapi_Float3D color{1, 1, 1};
      remixapi_Float3D direction{0, 0, -1};
      float intensity = 1, exposure = 0, temperature = 6500, radius = .5f, width = 1, height = 1, length = 1;
      float angle = .009250245f, cone = 3.14159265f, softness = 0, focus = 0;
      remixapi_Bool temperatureEnabled = false;
      std::filesystem::path texture;
    };
    struct LightDefinition {
      LightSource source;
      scene::DistantLight distant;
      std::vector<scene::SphereLight> emitters;
      scene::DomeLight dome;
    };
    std::unordered_map<uint64_t, LightDefinition> m_lights;
    FrameState m_frame, m_submitted;
  };
} // namespace remix
