// Optional Linux editor ABI; the Remix 0.1003 table is unchanged.
#pragma once
#include "remix_c.h"
#define REMIXAPI_STRUCT_TYPE_EDITOR_STARTUP_EXT ((remixapi_StructType)0x7fff1010)
typedef struct remixapi_EditorStartupEXT {
  remixapi_StructType sType;
  const void *pNext;
  uint32_t version; // 1
  remixapi_Bool headless;
  remixapi_Bool disableProfiles;
  remixapi_Bool disableMods;
} remixapi_EditorStartupEXT;
// Version 1 is the toolkit EditorSharedFrame contract. The caller owns all FDs.
// RGBA8 UNORM, optimal tiling, exclusive sharing, dedicated opaque-FD memory.
// Present waits for GPU completion before reporting success. Consumers wait on
// ready, acquire EXTERNAL -> their queue in GENERAL, then release to EXTERNAL
// in GENERAL and signal released. No second Present before the consumer releases.
typedef struct remixapi_EditorSharedFrame {
  uint32_t version, width, height, memory_type;
  uint64_t allocation_size;
  uint8_t device_uuid[16];
  int memory_fd, ready_fd, released_fd;
} remixapi_EditorSharedFrame;
#ifdef __cplusplus
extern "C" {
#endif
REMIXAPI int REMIXAPI_CALL remixapi_EditorConfigureSharedFrame(remixapi_EditorSharedFrame *frame);
#ifdef __cplusplus
}
#endif
