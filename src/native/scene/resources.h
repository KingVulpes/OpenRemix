#pragma once
#include "scene/retained.h"
#include "scene/temporal.h"
#include "scene/types.h"

#include <array>
#include <atomic>
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
  class NativeResources {
  public:
    NativeResources();
    ~NativeResources() = default;
    NativeResources(const NativeResources &) = delete;
    NativeResources &operator=(const NativeResources &) = delete;
    void reset();
    void clearModMaterials() {
      m_modMaterials.clear();
      ++m_modGeneration;
    }
    void replaceModMaterials(const std::map<uint64_t, remixapi_MaterialHandle> &handles) {
      std::unordered_map<uint64_t, std::shared_ptr<scene::Material>> next;
      for (auto [hash, handle] : handles)
        if (handle)
          next[hash] = m_materials.at(uint64_t(reinterpret_cast<uintptr_t>(handle)));
      m_modMaterials = std::move(next);
      ++m_modGeneration;
    }
    void invalidateTexturePaths() { m_paths.clear(); }
    void cacheTexturePath(const std::string &path, const std::shared_ptr<scene::Texture> &texture) {
      m_paths[std::filesystem::absolute(std::filesystem::u8path(path)).lexically_normal()] = texture;
    }
    void createMaterial(const remixapi_MaterialInfo &info, remixapi_MaterialHandle &handle);
    void setMaterialSourceHash(remixapi_MaterialHandle handle, uint64_t hash) {
      m_materials.at(uint64_t(reinterpret_cast<uintptr_t>(handle)))->sourceHash = hash;
      ++m_modGeneration;
    }
    void destroyMaterial(remixapi_MaterialHandle handle);
    void createMesh(const remixapi_MeshInfo &info, remixapi_MeshHandle &handle);
    void destroyMesh(remixapi_MeshHandle handle);
    void createTexture(const remixapi_TextureInfo &info, remixapi_TextureHandle &handle);
    // Stores pixels prepared outside the API mutex (see scene::compressTexture).
    void createTexture(const remixapi_TextureInfo &info, scene::Pixels &&pixels, remixapi_TextureHandle &handle);
    void destroyTexture(remixapi_TextureHandle handle);
    // "Keep BC textures compressed": applies to textures created afterwards.
    // Atomic so the API layer can choose the preparation path before locking.
    void setKeepCompressedTextures(bool value) { m_keepCompressed.store(value, std::memory_order_relaxed); }
    bool keepCompressedTextures() const { return m_keepCompressed.load(std::memory_order_relaxed); }
    // Honour remixapi_MeshInfoRefreshGeometryEXT ("Refresh mesh geometry in
    // place"). Off: the struct is ignored and a repeated CreateMesh replaces the
    // mesh as before.
    void setRefreshGeometry(bool value) { m_refreshGeometry = value; }
    bool refreshGeometry() const { return m_refreshGeometry; }
    void setupCamera(const remixapi_CameraInfo &info);
    void drawInstance(const remixapi_InstanceInfo &info, remixapi_MaterialHandle materialOverride = nullptr);
    // Retained instances (remix_fo4_ext.h). Handles are nonzero and never reused.
    uint64_t createRetained(const remixapi_InstanceInfo &info);
    void updateRetained(uint64_t handle, const remixapi_InstanceInfo &info);
    void updateRetained(const remixapi_RetainedInstanceUpdate *updates, uint32_t count);
    void destroyRetained(const uint64_t *handles, uint32_t count);
    void drawRetained(const float *sceneOrigin);
    size_t retainedCount() const { return m_retained.size(); }
    const scene::RetainedStatistics &retainedStatistics() const { return m_retainedStats; }
    // DrawRetainedInstances calls after which a moved instance returns to static.
    static constexpr uint64_t retainedSettleFrames = 240;
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
    const scene::Matrix &projection(uint32_t type = 0) const { return m_projections[type]; }
    const std::array<float, 4> &reverseDepth(uint32_t type = 0) const { return m_reverseDepth[type]; }
    bool cameraValid() const { return m_cameraValid[0]; }
    scene::TemporalEpoch temporalEpoch() const { return m_temporalEpoch; }
    const std::array<float, 3> &skyRadiance() const { return m_skyRadiance; }
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
    const scene::DomeLight &dome() const { return m_dome; }
    const scene::DistantLight &light() const { return m_light; }
    std::vector<scene::SphereLight> sphereLights() const;
    bool hasSphereLights() const { return !m_frameLights.empty(); }

  protected:
    std::shared_ptr<scene::Texture> texturePath(remixapi_Path path);
    std::shared_ptr<scene::Texture> dataTexturePath(remixapi_Path path);
    void storeTexture(uint64_t hash, scene::Pixels &&pixels, remixapi_TextureHandle &handle);
    std::atomic<bool> m_keepCompressed{false};
    bool m_refreshGeometry = false;
    std::shared_ptr<scene::Texture> m_white;
    std::shared_ptr<scene::Material> m_default;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Material>> m_materials, m_modMaterials;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Mesh>> m_meshes;
    std::unordered_map<uint64_t, std::shared_ptr<scene::Texture>> m_textures, m_uiTextures;
    std::unordered_map<std::filesystem::path, std::weak_ptr<scene::Texture>> m_paths;
    std::array<scene::Matrix, 3> m_cameras, m_views, m_projections;
    std::array<std::array<float, 4>, 3> m_reverseDepth;
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
    std::unordered_set<uint64_t> m_frameLights;
    scene::DistantLight m_light;
    scene::DomeLight m_dome;
    std::array<bool, 3> m_cameraValid = {};
    scene::TemporalEpoch m_temporalEpoch;
    std::array<float, 3> m_skyRadiance = {};
    std::vector<scene::Draw> m_draws, m_uiDraws, m_overlay;
    std::unordered_set<uint64_t> m_frameIds;
    float m_tint[4] = {};

    // Appends one Draw per (instance, surface) of mesh, as DrawInstance builds
    // them. strict applies DrawInstance's rule that a surface material handle
    // must be live; otherwise such a surface uses the default material and false
    // is returned. resolved receives the materials the surface handles named.
    bool buildDraws(const scene::InstanceDescription &description, const std::shared_ptr<scene::Mesh> &mesh,
                    const scene::Matrix *models, uint32_t count,
                    const std::shared_ptr<scene::Material> &materialOverride, bool strict,
                    std::vector<scene::Draw> &out, std::vector<std::shared_ptr<scene::Material>> *resolved);
    void resolveRetained(scene::RetainedInstance &entry, bool strict);
    bool retainedStale(const scene::RetainedInstance &entry) const;
    void setRetainedWatch(scene::RetainedInstance &entry, bool watch);
    void placeRetained(scene::RetainedInstance &entry);
    void classifyRetained(scene::RetainedInstance &entry, uint32_t classification);
    void moveRetained(scene::RetainedInstance &entry);
    // Dense table; m_retainedIndex maps a handle to its slot.
    std::vector<scene::RetainedInstance> m_retained;
    std::unordered_map<uint64_t, uint32_t> m_retainedIndex;
    std::vector<scene::Matrix> m_modelScratch;
    scene::RetainedStatistics m_retainedStats;
    uint64_t m_nextRetained = 0, m_retainedSerial = 0;
    // Bumped when a mesh or material handle starts resolving to a new object,
    // and when the mod material mapping changes.
    uint64_t m_resourceGeneration = 0, m_modGeneration = 0;
    uint64_t m_retainedResourceGeneration = 0, m_retainedModGeneration = 0;
    uint32_t m_retainedWatch = 0;
  };
} // namespace remix
