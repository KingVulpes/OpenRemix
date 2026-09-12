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
#include <cmath>

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
    if (!std::strcmp(key, "upscaler")) {
      if (std::strcmp(value, "off") && std::strcmp(value, "fsr31"))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      m_traceOptions.fsr = !std::strcmp(value, "fsr31");
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    if (!std::strcmp(key, "fsrQuality")) {
      const char* modes[] = {"native-aa", "quality", "balanced", "performance", "ultra-performance"};
      for (uint32_t i = 0; i < 5; ++i) {
        if (!std::strcmp(value, modes[i])) {
          m_traceOptions.fsrQuality = i;
          return REMIXAPI_ERROR_CODE_SUCCESS;
        }
      }
      return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    }
    if (!std::strcmp(key, "fsrSharpness")) {
      float sharpness = 0;
      const char* end = value + std::strlen(value);
      auto parsed = std::from_chars(value, end, sharpness);
      if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(sharpness) || sharpness < 0 || sharpness > 1)
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      m_traceOptions.fsrSharpness = sharpness;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
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
    if (!std::strcmp(key, "localIntensity") || !std::strcmp(key, "localRange")) {
      const bool range = !std::strcmp(key, "localRange");
      float intensity = 0;
      const char *end = value + std::strlen(value);
      auto parsed = std::from_chars(value, end, intensity);
      if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(intensity) || intensity < 0 || intensity > (range ? 1024 : 100) ||
          (range && intensity > 0 && intensity < .01f))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      (range ? m_traceOptions.localRange : m_traceOptions.localIntensity) = intensity;
      return REMIXAPI_ERROR_CODE_SUCCESS;
    }
    if (!std::strcmp(key, "sunAngularDiameter")) {
      float diameter = 0;
      const char *end = value + std::strlen(value);
      auto parsed = std::from_chars(value, end, diameter);
      if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(diameter) ||
          (diameter != -1 && (diameter < 0 || diameter > 10)))
        return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
      m_traceOptions.sunAngularDiameter = diameter;
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
    else if (!std::strcmp(key, "sigma") && number <= 1)
      m_traceOptions.sigma = number != 0;
    else if (!std::strcmp(key, "prism") && number <= 1)
      m_traceOptions.prism = number != 0;
    else if (!std::strcmp(key, "gamma22") && number <= 1)
      m_traceOptions.gamma22 = number != 0;
    else if (!std::strcmp(key, "localLights") && number <= 1)
      m_traceOptions.localLights = number != 0;
    else if (!std::strcmp(key, "torchLights") && number <= 1)
      m_traceOptions.torchLights = number != 0;
    else if (!std::strcmp(key, "emissiveLights") && number <= 1)
      m_traceOptions.emissiveLights = number != 0;
    else if (!std::strcmp(key, "emissiveCandidates") && number >= 1 && number <= 8)
      m_traceOptions.emissiveCandidates = number;
    else if (!std::strcmp(key, "separateLocalLighting") && number <= 2)
      m_traceOptions.separateLocalLighting = number;
    else if (!std::strcmp(key, "localDenoiser") && number <= 1)
      m_traceOptions.localDenoiser = number != 0;
    else if (!std::strcmp(key, "localGuiding") && number <= 1)
      m_traceOptions.localGuiding = number != 0;
    else if (!std::strcmp(key, "localSamples") && number >= 1 && number <= 16)
      m_traceOptions.localSamples = number;
    else if (!std::strcmp(key, "shadowDebug") && number <= 2)
      m_traceOptions.shadowDebug = number;
    else if (!std::strcmp(key, "taa") && number <= 1)
      m_traceOptions.taa = number != 0;
    else if (!std::strcmp(key, "waterIndirectReuse") && number <= 1)
      m_traceOptions.waterIndirectReuse = number != 0;
    else if (!std::strcmp(key, "validation") && number <= 1)
      m_traceOptions.validation = number != 0;
    else if (!std::strcmp(key, "sharc") && number <= 1)
      m_traceOptions.sharc = number != 0;
    else if (!std::strcmp(key, "sharcDebug") && number <= 1)
      m_traceOptions.sharcDebug = number != 0;
    else if (!std::strcmp(key, "sharcReset") && number == 1)
      ++m_traceOptions.sharcReset;
    else if (!std::strcmp(key, "sharcCapacityLog2") && number >= 16 && number <= 22)
      m_traceOptions.sharcCapacityLog2 = number;
    else if (!std::strcmp(key, "sharcDownscale") && number >= 1 && number <= 16)
      m_traceOptions.sharcDownscale = number;
    else if (!std::strcmp(key, "sharcSceneScale") && number >= 1 && number <= 200)
      m_traceOptions.sharcSceneScale = number;
    else if (!std::strcmp(key, "sharcAccumulationFrames") && number >= 1 && number <= 1024)
      m_traceOptions.sharcAccumulationFrames = number;
    else if (!std::strcmp(key, "sharcStaleFrames") && number >= 8 && number <= 1024)
      m_traceOptions.sharcStaleFrames = number;
    else if (!std::strcmp(key, "sky") && number <= 1)
      m_traceOptions.sky = number != 0;
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
    result.redrawRequested = (m_backend && (result.redrawRequested || m_traceOptions.accumulate)) ||
                             m_backend != m_requestedBackend;
    return result;
  }
  remixapi_SharcStatisticsEXT RemixNativeRuntime::sharcStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->sharcStatistics()
                              : remixapi_SharcStatisticsEXT{.structSize = sizeof(remixapi_SharcStatisticsEXT)};
    result.requested = m_traceOptions.sharc;
    if (!m_pathTracer)
      std::strncpy(result.unavailableReason, REMIX_HAS_SHARC ? "Path tracing is inactive" : "SHARC was disabled at build time",
                   sizeof(result.unavailableReason) - 1);
    return result;
  }
  remixapi_LocalLightGuidingStatisticsEXT RemixNativeRuntime::localLightGuidingStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->localLightGuidingStatistics()
        : remixapi_LocalLightGuidingStatisticsEXT{.structSize = sizeof(remixapi_LocalLightGuidingStatisticsEXT)};
    result.requested = m_traceOptions.localGuiding;
    result.influenceRange = m_traceOptions.localRange;
    return result;
  }
  remixapi_LocalLightStatisticsEXT RemixNativeRuntime::localLightStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->localLightStatistics()
        : remixapi_LocalLightStatisticsEXT{.structSize = sizeof(remixapi_LocalLightStatisticsEXT)};
    result.enabled = m_traceOptions.localLights;
    result.samples = m_traceOptions.localSamples;
    return result;
  }
  remixapi_ShadowStatisticsEXT RemixNativeRuntime::shadowStatistics() const {
    auto result = m_pathTracer ? m_pathTracer->shadowStatistics()
                              : remixapi_ShadowStatisticsEXT{.structSize = sizeof(remixapi_ShadowStatisticsEXT)};
    result.requested = m_traceOptions.sigma;
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
