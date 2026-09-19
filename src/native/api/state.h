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

#pragma once
#include "api/exports.h"
#include "runtime/runtime.h"
#include <mutex>
#include <string>
#include <unordered_map>

namespace remix::api {
  extern remix::RemixNativeRuntime g_runtime;
  extern std::mutex g_apiMutex;
  extern bool g_frameExitRegistered;
  extern PFN_remixapi_BridgeCallback g_beginSceneCallback;
  extern PFN_remixapi_BridgeCallback g_endSceneCallback;
  extern PFN_remixapi_BridgeCallback g_presentCallback;
  extern std::unordered_map<std::string, std::string> g_gameValues;

  template <typename F> remixapi_ErrorCode resourceCall(F &&call) {
    std::lock_guard lock(g_apiMutex);
    if (!g_runtime.isInitialized()) {
      return REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    }
    if (g_runtime.hasFailed()) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    try {
      call(g_runtime.raster());
      return REMIXAPI_ERROR_CODE_SUCCESS;
    } catch (remixapi_ErrorCode code) {
      return code;
    } catch (...) {
      return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
  }

  remixapi_ErrorCode ignoredEffect();
  remixapi_ErrorCode REMIXAPI_CALL requireRenderer();
} // namespace remix::api
