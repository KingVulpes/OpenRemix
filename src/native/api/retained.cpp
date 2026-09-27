// Retained instances (remix_fo4_ext.h), after the dxvk-remix fork
// (Kim2091/dxvk-remix rebased-9-19-opt). Named exports only; the versioned
// interface table is unchanged. Hosts resolve them with GetProcAddress/dlsym.
// Validation, resolution and replay live in scene/retained.cpp. Every call
// takes the API mutex like the other entry points. Error codes follow the fork,
// except that an uninitialized runtime reports NOT_INITIALIZED as the other
// OpenRemix entry points do (the fork: REMIX_DEVICE_WAS_NOT_REGISTERED).

#include "api/state.h"

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_CreateRetainedInstance(const remixapi_InstanceInfo *info,
                                                                          uint64_t *outHandle) {
  if (!outHandle)
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  return resourceCall([&](auto &resources) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO)
      throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    *outHandle = resources.createRetained(*info);
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_UpdateRetainedInstance(uint64_t handle,
                                                                          const remixapi_InstanceInfo *info) {
  return resourceCall([&](auto &resources) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_INSTANCE_INFO)
      throw REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    resources.updateRetained(handle, *info);
  });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL
remixapi_UpdateRetainedInstances(const remixapi_RetainedInstanceUpdate *updates, uint32_t count) {
  return resourceCall([&](auto &resources) { resources.updateRetained(updates, count); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DestroyRetainedInstances(const uint64_t *handles,
                                                                            uint32_t count) {
  return resourceCall([&](auto &resources) { resources.destroyRetained(handles, count); });
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_DrawRetainedInstances(const float *sceneOrigin) {
  return resourceCall([&](auto &resources) { resources.drawRetained(sceneOrigin); });
}
}
