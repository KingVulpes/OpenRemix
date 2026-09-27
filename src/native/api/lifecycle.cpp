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
#if REMIX_HAS_USD
#include "usd/mods.h"
#endif
#include "api/entrypoints.h"
#include <cstdlib>

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Startup(const remixapi_StartupInfo *info) {
  if (info == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_STARTUP_INFO) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  auto lock = lockApi();
  const remixapi_ErrorCode status = g_runtime.initialize(*info);
  if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
    g_frameExitStages = 0;
#if REMIX_HAS_USD
    try {
      remix::usd::startMods(g_runtime.modsDisabled());
    } catch (...) {
      remix::usd::stopMods(g_runtime.raster());
      g_runtime.shutdown();
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
#endif
  }
#if !defined(_WIN32)
  // Register after SDL startup so this runs before SDL's process-exit window cleanup.
  if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
    if (std::atexit([] { remixapi_Shutdown(); }) != 0) {
#if REMIX_HAS_USD
      remix::usd::stopMods(g_runtime.raster());
#endif
      g_runtime.shutdown();
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
  }
#endif
  return status;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Shutdown() {
  releaseUsdSubmissions();
  auto lock = lockApi();
#if REMIX_HAS_USD
  remix::usd::stopMods(g_runtime.raster());
#endif
  remixapi_ErrorCode status = g_runtime.shutdown();
  if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
    g_beginSceneCallback = nullptr;
    g_endSceneCallback = nullptr;
    g_presentCallback = nullptr;
  }
  return status;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Present(const remixapi_PresentInfo *info) {
  PFN_remixapi_BridgeCallback presentCallback = nullptr;
  remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  {
    std::lock_guard lock(g_apiMutex);
#if !defined(_WIN32)
    // Validation and reconstruction SDKs initialize process-static resources on
    // first use. Async preparation can activate them after the initial raster
    // frame. Renew cleanup after each renderer's first use so our GPU wait and
    // resource release precede the SDK's process-exit destructors. The mask
    // bounds registrations per startup, independently of frame count/resizes.
    // Asynchronous presentation reads renderer state before handing off the
    // next frame, once the previous one has finished rendering.
    auto registerExit = [] {
      uint32_t exitStages = g_runtime.hasStartedFrame() ? 1u : 0u;
      if (g_runtime.backendCapabilities().activeBackend == 1)
        exitStages |= 2u;
      const auto reconstruction = g_runtime.reconstructionStatistics();
      if (reconstruction.frames)
        exitStages |= 4u << uint32_t(reconstruction.backend);
      if (exitStages & ~g_frameExitStages) {
        if (std::atexit([] { remixapi_Shutdown(); }) != 0)
          return false;
        g_frameExitStages |= exitStages;
      }
      return true;
    };
    if (g_runtime.asyncPresent()) {
      g_runtime.waitRenderIdle();
      if (!registerExit()) {
        g_runtime.shutdown();
        return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
      }
    }
#endif
    status = g_runtime.present(info);
#if REMIX_HAS_USD
    if (status == REMIXAPI_ERROR_CODE_SUCCESS)
      remix::usd::modBoundary(g_runtime.raster());
#endif
#if !defined(_WIN32)
    if (!g_runtime.asyncPresent() && !registerExit()) {
      g_runtime.shutdown();
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
#endif
    presentCallback = g_presentCallback;
  }
  if (status == REMIXAPI_ERROR_CODE_SUCCESS)
    publishUsdReloads();
  static thread_local bool inCallback = false;
  if (status == REMIXAPI_ERROR_CODE_SUCCESS && presentCallback && !inCallback) {
    inCallback = true;
    try {
      presentCallback();
    } catch (...) {
      inCallback = false;
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    inCallback = false;
  }
  return status;
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterCallbacks(
    PFN_remixapi_BridgeCallback beginSceneCallback, PFN_remixapi_BridgeCallback endSceneCallback,
    PFN_remixapi_BridgeCallback presentCallback) {
  auto lock = lockApi();
  g_beginSceneCallback = beginSceneCallback;
  g_endSceneCallback = endSceneCallback;
  g_presentCallback = presentCallback;
  return REMIXAPI_ERROR_CODE_SUCCESS;
}
}
