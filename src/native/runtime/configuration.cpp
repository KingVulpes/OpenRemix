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

#include "runtime/runtime.h"
#include "upscale/ngx.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <charconv>
#include <cmath>

namespace remix {
  remixapi_ErrorCode RemixNativeRuntime::selectBackend(const char *value) {
    auto next = m_profiles.current();
    if (config::set(next, "rtx.native.backend", value) != config::SetResult::Success)
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (next.backend && !m_traceSupported) {
      SDL_Log("Path tracing unavailable: %s", m_traceUnavailable.c_str());
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
    m_backendFailed = false;
    m_profiles.edit(next);
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::configureSetting(const char *key, const char *value) {
    auto next = m_profiles.current();
    if (config::set(next, key, value) != config::SetResult::Success || !m_profiles.edit(next))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::configureTracing(const char *key, const char *value) {
    if (!std::strcmp(key, "sharcReset")) {
      unsigned number = 0;
      const auto end = value + std::strlen(value);
      auto parsed = std::from_chars(value, end, number);
      if (std::strcmp(value, "true") && (parsed.ec != std::errc{} || parsed.ptr != end || number != 1))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      ++m_sharcReset;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    return configureSetting((std::string("rtx.native.pathtrace.") + key).c_str(), value);
  }

  void RemixNativeRuntime::applySettings() {
    const auto &settings = m_profiles.current();
    if (m_requestedBackend != settings.backend) m_backendFailed = false;
    m_requestedBackend = settings.backend;
    m_targetBackend = m_traceSupported && !m_backendFailed ? settings.backend : 0;
    m_appliedSettings = settings;
    m_traceOptions = settings.trace;
    m_traceOptions.sharcReset = m_sharcReset;
    m_vsync = settings.vsync;
    std::copy(settings.clearColor.begin(), settings.clearColor.end(), m_clearColor);
  }

  upscale::Statistics RemixNativeRuntime::reconstructionStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->upscalingStatistics() : upscale::Statistics{};
    result.requested = m_profiles.current().trace.upscaler;
    if (m_ngx) {
      const auto caps = m_ngx->capabilities();
      result.srSupported = caps.sr; result.rrSupported = caps.rr; result.sdkAvailable = caps.sdk;
      if (result.failure.empty() && !result.active) result.failure = caps.reason;
    }
    return result;
  }
  remixapi_BackendCapabilitiesEXT RemixNativeRuntime::backendCapabilities() const {
    remixapi_BackendCapabilitiesEXT result = {.structSize = sizeof(result),
                                              .supportedBackends = m_traceSupported ? 3u : 1u,
                                              .activeBackend = m_backend,
                                              .requestedBackend = m_profiles.current().backend};
    std::strncpy(result.unavailableReason, m_traceUnavailable.c_str(), sizeof(result.unavailableReason) - 1);
    return result;
  }

  remixapi_TracingStatisticsEXT RemixNativeRuntime::tracingStatistics() const {
    auto result = m_pathTracer
                      ? m_pathTracer->statistics()
                      : remixapi_TracingStatisticsEXT{.structSize = sizeof(remixapi_TracingStatisticsEXT)};
    result.redrawRequested = (m_backend && (result.redrawRequested || m_profiles.current().trace.accumulate)) ||
                             m_backend != m_targetBackend || m_profiles.current() != m_appliedSettings;
    return result;
  }
  remixapi_SharcStatisticsEXT RemixNativeRuntime::sharcStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->sharcStatistics()
                              : remixapi_SharcStatisticsEXT{.structSize = sizeof(remixapi_SharcStatisticsEXT)};
    result.requested = m_profiles.current().trace.sharc;
    if (!m_pathTracer)
      std::strncpy(result.unavailableReason, REMIX_HAS_SHARC ? "Path tracing is inactive" : "SHARC was disabled at build time",
                   sizeof(result.unavailableReason) - 1);
    return result;
  }
  remixapi_LocalLightGuidingStatisticsEXT RemixNativeRuntime::localLightGuidingStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->localLightGuidingStatistics()
        : remixapi_LocalLightGuidingStatisticsEXT{.structSize = sizeof(remixapi_LocalLightGuidingStatisticsEXT)};
    result.requested = m_profiles.current().trace.localGuiding;
    result.influenceRange = m_profiles.current().trace.localRange;
    return result;
  }
  remixapi_LocalLightStatisticsEXT RemixNativeRuntime::localLightStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->localLightStatistics()
        : remixapi_LocalLightStatisticsEXT{.structSize = sizeof(remixapi_LocalLightStatisticsEXT)};
    result.enabled = m_profiles.current().trace.localLights;
    result.samples = m_profiles.current().trace.localSamples;
    return result;
  }
  remixapi_ShadowStatisticsEXT RemixNativeRuntime::shadowStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->shadowStatistics()
                              : remixapi_ShadowStatisticsEXT{.structSize = sizeof(remixapi_ShadowStatisticsEXT)};
    result.requested = m_profiles.current().trace.sigma;
    return result;
  }
  remixapi_DenoisingStatisticsEXT RemixNativeRuntime::denoisingStatistics() const {
    auto result =
        m_pathTracer ? m_pathTracer->denoisingStatistics()
                     : remixapi_DenoisingStatisticsEXT{.structSize = sizeof(remixapi_DenoisingStatisticsEXT)};
    result.requestedDenoiser = m_profiles.current().trace.denoiser;
    result.requestedTaa = m_profiles.current().trace.taa;
    if (!m_pathTracer && ((m_profiles.current().trace.denoiser && !m_denoiseCapabilities.reblurShSupported) ||
                          (m_profiles.current().trace.taa && !m_denoiseCapabilities.taaSupported)))
      std::strncpy(result.unavailableReason, m_denoiseCapabilities.unavailableReason,
                   sizeof(result.unavailableReason) - 1);
    return result;
  }
  remixapi_FeatureAvailability RemixNativeRuntime::getFeatureAvailability() const {
    remixapi_FeatureAvailability result{};
#ifdef REMIX_HAS_NRD
    result.compiledFeatures = REMIXAPI_FEATURE_NRD_BIT;
    if (m_denoiseCapabilities.reblurShSupported)
      result.availableFeatures = REMIXAPI_FEATURE_NRD_BIT;
#endif
    return result;
  }
} // namespace remix
