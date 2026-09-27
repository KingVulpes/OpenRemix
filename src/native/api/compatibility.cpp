/*
 * Copyright (c) 2026, NVIDIA CORPORATION. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include "api/state.h"
#include "api/entrypoints.h"

namespace remix::api {
  remixapi_ErrorCode REMIXAPI_CALL requireRenderer() {
    auto lock = lockApi();
    return g_runtime.isInitialized() ? REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE
                                     : REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
  }

  remixapi_ErrorCode REMIXAPI_CALL setCameraMediumMaterial(const remixapi_CameraMediumInfo *info) {
    if (!info || info->sType != REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return ignoredEffect();
  }

  remixapi_ErrorCode REMIXAPI_CALL addTextureHash(const char *category, const char *hash) {
    if (!category || !*category || !hash || !*hash) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return ignoredEffect();
  }

  remixapi_ErrorCode REMIXAPI_CALL removeTextureHash(const char *category, const char *hash) {
    if (!category || !*category || !hash || !*hash) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return ignoredEffect();
  }

  remixapi_ErrorCode REMIXAPI_CALL
  requestObjectPicking(const remixapi_Rect2D *pixelRegion,
                       PFN_remixapi_pick_RequestObjectPickingUserCallback callback, void *) {
    if (pixelRegion == nullptr || callback == nullptr) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return requireRenderer();
  }

  remixapi_ErrorCode REMIXAPI_CALL highlightObjects(const uint32_t *values, uint32_t valueCount, uint8_t,
                                                    uint8_t, uint8_t) {
    if (valueCount != 0 && values == nullptr) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return requireRenderer();
  }

  remixapi_ErrorCode REMIXAPI_CALL requestTextureVramFree() { return ignoredEffect(); }

  remixapi_ErrorCode REMIXAPI_CALL requestVramCompaction() { return ignoredEffect(); }

  remixapi_ErrorCode REMIXAPI_CALL getVramStats(remixapi_VramStats *stats) {
    if (stats == nullptr) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    *stats = {};
    return requireRenderer();
  }

#if defined(_WIN32)
  remixapi_ErrorCode REMIXAPI_CALL createD3D9(remixapi_Bool, IDirect3D9Ex **output) {
    if (!output) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    *output = nullptr;
    return requireRenderer();
  }

  remixapi_ErrorCode REMIXAPI_CALL registerD3D9(IDirect3DDevice9Ex *device) {
    if (!device) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return requireRenderer();
  }

  remixapi_ErrorCode REMIXAPI_CALL getExternalSwapchain(uint64_t *image, uint64_t *done, uint64_t *resume) {
    if (image) {
      *image = 0;
    }
    if (done) {
      *done = 0;
    }
    if (resume) {
      *resume = 0;
    }
    if (!image || !done || !resume) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return requireRenderer();
  }

  remixapi_ErrorCode REMIXAPI_CALL getVkImage(IDirect3DSurface9 *source, uint64_t *output) {
    if (output) {
      *output = 0;
    }
    if (!source || !output) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return requireRenderer();
  }

  remixapi_ErrorCode REMIXAPI_CALL copyOutput(IDirect3DSurface9 *target,
                                              remixapi_dxvk_CopyRenderingOutputType type) {
    if (!target || type < 0 || type > REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_GUI) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return requireRenderer();
  }

  remixapi_ErrorCode REMIXAPI_CALL defaultOutput(remixapi_dxvk_CopyRenderingOutputType type,
                                                 const remixapi_Float4D *color) {
    if (!color || type < 0 || type > REMIXAPI_DXVK_COPY_RENDERING_OUTPUT_TYPE_GUI) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return requireRenderer();
  }

  remixapi_ErrorCode REMIXAPI_CALL getTextureHash(IDirect3DTexture9 *texture, uint64_t *output) {
    if (output) {
      *output = 0;
    }
    if (!texture || !output) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return requireRenderer();
  }

  remixapi_ErrorCode REMIXAPI_CALL getSharedTexture(void **handle, uint32_t *width, uint32_t *height) {
    if (handle) {
      *handle = nullptr;
    }
    if (width) {
      *width = 0;
    }
    if (height) {
      *height = 0;
    }
    if (!handle || !width || !height) {
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    return requireRenderer();
  }

#endif
  remixapi_ErrorCode ignoredEffect() {
    return resourceCall([](auto &) {});
  }

} // namespace remix::api

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetFogState(const remixapi_FogInfo *info) {
  if (!info || info->sType != REMIXAPI_STRUCT_TYPE_FOG_INFO) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  return ignoredEffect();
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_AutoInstancePersistentLights() { return ignoredEffect(); }
}
