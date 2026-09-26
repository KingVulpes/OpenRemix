#pragma once
#include "scene/resources.h"
#include "usd/loader.h"
#include <remix/remix_native_ext.h>
#include <set>

namespace remix::usd {
  // CPU resources only. Draws hold shared ownership while GPU retirement proceeds.
  class Submission {
  public:
    explicit Submission(NativeResources &resources, const Snapshot &snapshot, uint64_t namespaceId,
                        bool applyMods = true);
    ~Submission();
    void retireAttachments();
    void draw(bool camera, float aspect, const remixapi_InstanceInfo *parent = nullptr,
              const std::string &root = "");
    remixapi_MaterialHandle materialHandle(const std::string &id) const {
      auto it = materials_.find(id);
      return it == materials_.end() ? nullptr : it->second;
    }
    const std::vector<remixapi_InstanceInfo> &instances() const { return instances_; }

  private:
    void clear();
    void material(const Json &);
    void mesh(const Json &);
    void mesh_group(const Json &, uint32_t);
    void add_instance(const Json &, remixapi_MeshHandle, uint32_t);
    void add_light(const Json &);
    remixapi_LightHandle create_light(const Json &, const remixapi_Transform &);
    NativeResources &resources_;
    uint64_t namespace_;
    bool applyMods_;
    std::vector<uint64_t> instance_hashes_;
    Json snapshot_;
    std::map<std::string, remixapi_MaterialHandle> materials_;
    std::vector<remixapi_MeshHandle> meshes_;
    std::map<uint64_t, remixapi_MeshHandle> mesh_cache_;
    std::vector<remixapi_InstanceInfo> instances_;
    std::vector<remixapi_InstanceInfoBlendEXT> blend_states_;
    std::vector<std::pair<std::string, remixapi_Transform>> instance_origins_;
    std::vector<remixapi_LightHandle> lights_;
    std::vector<Json> light_origins_;
    std::set<remixapi_LightHandle> attached_lights_, used_attachments_;
  };
} // namespace remix::usd
