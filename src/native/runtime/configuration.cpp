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

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <charconv>

namespace remix {
  remixapi_ErrorCode RemixNativeRuntime::selectBackend(const char *value) {
    if (!std::strcmp(value, "raster")) {
      m_requestedBackend = 0;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    if (std::strcmp(value, "pathtrace"))
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    if (!m_traceSupported) {
      SDL_Log("Path tracing unavailable: %s", m_traceUnavailable.c_str());
      return REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    }
    m_requestedBackend = 1;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_ErrorCode RemixNativeRuntime::configureTracing(const char *key, const char *value) {
    if (!std::strcmp(key, "primaryVisibility")) {
      if (std::strcmp(value, "raster") && std::strcmp(value, "ray"))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      m_traceOptions.rasterPrimary = !std::strcmp(value, "raster");
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    if (!std::strcmp(key, "denoiser")) {
      if (std::strcmp(value, "off") && std::strcmp(value, "reblur-sh"))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      m_traceOptions.denoiser = !std::strcmp(value, "reblur-sh");
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    unsigned number = 0;
    const char *end = value + std::strlen(value);
    auto parsed = std::from_chars(value, end, number);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
      if (!std::strcmp(value, "true"))
        number = 1;
      else if (!std::strcmp(value, "false"))
        number = 0;
      else
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!std::strcmp(key, "samples") && number >= 1 && number <= 16)
      m_traceOptions.samples = number;
    else if (!std::strcmp(key, "bounces") && number <= 4)
      m_traceOptions.bounces = number;
    else if (!std::strcmp(key, "accumulate") && number <= 1)
      m_traceOptions.accumulate = number != 0;
    else if (!std::strcmp(key, "taa") && number <= 1)
      m_traceOptions.taa = number != 0;
    else if (!std::strcmp(key, "validation") && number <= 1)
      m_traceOptions.validation = number != 0;
    else if (!std::strcmp(key, "forceRebuild") && number <= 1)
      m_traceOptions.forceRebuild = number != 0;
    else
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  }

  remixapi_BackendCapabilitiesEXT RemixNativeRuntime::backendCapabilities() const {
    remixapi_BackendCapabilitiesEXT result = {.structSize = sizeof(result),
                                              .supportedBackends = m_traceSupported ? 3u : 1u,
                                              .activeBackend = m_backend,
                                              .requestedBackend = m_requestedBackend};
    std::strncpy(result.unavailableReason, m_traceUnavailable.c_str(), sizeof(result.unavailableReason) - 1);
    return result;
  }

  remixapi_TracingStatisticsEXT RemixNativeRuntime::tracingStatistics() const {
    auto result = m_pathTracer
                      ? m_pathTracer->statistics()
                      : remixapi_TracingStatisticsEXT{.structSize = sizeof(remixapi_TracingStatisticsEXT)};
    result.redrawRequested = (m_backend && m_traceOptions.accumulate) || m_backend != m_requestedBackend;
    return result;
  }
  remixapi_DenoisingStatisticsEXT RemixNativeRuntime::denoisingStatistics() const {
    auto result =
        m_pathTracer ? m_pathTracer->denoisingStatistics()
                     : remixapi_DenoisingStatisticsEXT{.structSize = sizeof(remixapi_DenoisingStatisticsEXT)};
    result.requestedDenoiser = m_traceOptions.denoiser;
    result.requestedTaa = m_traceOptions.taa;
    if (!m_pathTracer && ((m_traceOptions.denoiser && !m_denoiseCapabilities.reblurShSupported) ||
                          (m_traceOptions.taa && !m_denoiseCapabilities.taaSupported)))
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
