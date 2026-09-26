#pragma once
#include <remix/remix_c.h>
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
} // namespace remix::usd
