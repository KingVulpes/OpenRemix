#pragma once
#include <remix/remix_c.h>
#include <string>
#include <vector>
namespace remix {
  class NativeResources;
}
namespace remix::usd {
  void startMods(bool disabled);
  void stopMods(NativeResources &);
  void modBoundary(NativeResources &);
  bool modInstance(NativeResources &, const remixapi_InstanceInfo &, uint64_t originalHash = 0);
  bool modLight(NativeResources &, remixapi_LightHandle, uint64_t originalHash = 0);
  remixapi_ErrorCode configureMods(const char *directory, double meters, uint32_t axis, bool watch);
  const char *modDiagnostic();

  // Developer panel view of the loaded mods. Toggles keep every mod loaded:
  // they change which replacements apply from the next frame on.
  struct ModInfo {
    std::string name; // mod directory relative to the mods directory
    size_t meshes = 0, lights = 0, materials = 0;
    bool enabled = true;
  };
  struct ModStatus {
    bool active = false;  // mod loading started
    bool enabled = true;  // replacements applied at all
    bool loading = false; // a parse is in flight
    std::string directory, diagnostic;
    std::vector<ModInfo> mods;
  };
  ModStatus modStatus();
  void setModsEnabled(bool enabled);
  void setModEnabled(const std::string &name, bool enabled);
  void reloadMods();
} // namespace remix::usd
