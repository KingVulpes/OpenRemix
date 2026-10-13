#include "api/state.h"
#if REMIX_HAS_USD
#include "usd/mods.h"
#endif
#include "api/entrypoints.h"
#include "platform/gpu_timings.h"
#include "runtime/profiles.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

using namespace remix::api;

extern "C" {

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Startup(const remixapi_StartupInfo *info) {
  if (info == nullptr || info->sType != REMIXAPI_STRUCT_TYPE_STARTUP_INFO) {
    return REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  }
  try {
    std::lock_guard lock(g_apiMutex);
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
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Shutdown() {
  try {
    releaseUsdSubmissions();
    std::lock_guard lock(g_apiMutex);
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
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

namespace {
// Dev aid for unattended testing, polled every 30 presents. Files in the
// profile directory (REMIX_CONFIG_DIR, else ./openremix):
//   config.request     "key = value" lines applied as SetConfigVariable
//   screenshot.request presented frame to screenshot.png
//   timings.request    GPU pass timings (rtx.native.gpuPassTimers) to timings.txt
//   sequence.request   N (default 60) consecutive captures to sequence/NNN.png,
//                      one per present while the previous one has completed
// Each request file is deleted when served.
void serviceDevRequests() {
  static uint32_t calls = 0, sequenceLeft = 0, sequenceIndex = 0;
  std::error_code ec;
  static const auto directory = remix::config::Profiles::defaultDirectory();
  if (sequenceLeft) {
    char name[32];
    std::snprintf(name, sizeof(name), "%03u.png", sequenceIndex);
    const auto path = std::filesystem::absolute(directory / "sequence" / name, ec).string();
    std::lock_guard lock(g_apiMutex);
    if (g_runtime.requestScreenshot(path.c_str()) == REMIXAPI_ERROR_CODE_SUCCESS) {
      ++sequenceIndex;
      --sequenceLeft;
    }
  }
  if (++calls % 30)
    return;
  auto take = [&](const char *name, std::string *contents) {
    const auto path = directory / name;
    if (!std::filesystem::exists(path, ec))
      return false;
    if (contents) {
      std::ifstream in(path);
      std::stringstream text;
      text << in.rdbuf();
      *contents = text.str();
    }
    std::filesystem::remove(path, ec);
    return true;
  };
  std::string config;
  if (take("config.request", &config)) {
    std::istringstream lines(config);
    for (std::string line; std::getline(lines, line);) {
      const auto eq = line.find('=');
      if (eq == std::string::npos)
        continue;
      auto trim = [](std::string v) {
        const auto a = v.find_first_not_of(" \t\r"), b = v.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : v.substr(a, b - a + 1);
      };
      const auto key = trim(line.substr(0, eq)), value = trim(line.substr(eq + 1));
      if (!key.empty())
        remix::api::setConfigVariable(key.c_str(), value.c_str());
    }
  }
  if (take("screenshot.request", nullptr)) {
    const auto path = std::filesystem::absolute(directory / "screenshot.png", ec).string();
    std::lock_guard lock(g_apiMutex);
    g_runtime.requestScreenshot(path.c_str());
  }
  std::string sequence;
  if (!sequenceLeft && take("sequence.request", &sequence)) {
    std::filesystem::remove_all(directory / "sequence", ec);
    std::filesystem::create_directories(directory / "sequence", ec);
    sequenceLeft = std::clamp(std::atoi(sequence.c_str()), 0, 600);
    if (!sequenceLeft)
      sequenceLeft = 60;
    sequenceIndex = 0;
  }
  if (take("timings.request", nullptr)) {
    std::string text;
    {
      std::lock_guard lock(g_apiMutex);
      const auto &t = g_runtime.gpuPassTimings();
      char line[200];
      if (!t.available) {
        text = "unavailable (enable rtx.native.gpuPassTimers)\n";
      } else {
        std::snprintf(line, sizeof(line), "GPU frame %.3f ms (passes %.3f, between passes %.3f); render %ux%u, output %ux%u\n",
                      t.frameMilliseconds, t.passMilliseconds, t.untimedMilliseconds, t.renderWidth, t.renderHeight,
                      t.outputWidth, t.outputHeight);
        text += line;
        for (const auto index : remix::gpu::byCost(t)) {
          const auto &pass = t.passes[index];
          std::snprintf(line, sizeof(line), "%-56s %8.3f %8.3f\n", pass.name, pass.selfMilliseconds, pass.milliseconds);
          text += line;
        }
        const auto local = g_runtime.localLightStatistics();
        std::snprintf(line, sizeof(line), "# local lights %u (active %u), light tree nodes %u, depth %u\n", local.lightCount,
                      local.activeLights, local.nodeCount, local.treeDepth);
        text += line;
        const auto cache = g_runtime.sharcStatistics();
        std::snprintf(line, sizeof(line), "# sharc update paths %u, query attempts %u, hits %u, entries %u\n", cache.updatePaths,
                      cache.queryAttempts, cache.queryHits, cache.occupiedEntries);
        text += line;
        const auto breakdown = g_runtime.frameBreakdown();
        std::snprintf(line, sizeof(line),
                      "# raster draws %llu, outside the view (not drawn) %llu, prepare %.3f ms, record %.3f ms\n",
                      (unsigned long long)breakdown.rasterDraws, (unsigned long long)breakdown.rasterCulledDraws,
                      breakdown.rasterPrepareMilliseconds, breakdown.compositeMilliseconds);
        text += line;
        const auto raster = g_runtime.rasterProfile();
        std::snprintf(line, sizeof(line),
                      "# raster prepare %.3f ms: retire %.3f, texture refresh %.3f (%llu), order %.3f, draws %.3f "
                      "(%llu new meshes, %llu new materials)\n",
                      raster.prepareMilliseconds, raster.retireMilliseconds, raster.refreshMilliseconds,
                      (unsigned long long)raster.texturesRefreshed, raster.orderMilliseconds, raster.buildMilliseconds,
                      (unsigned long long)raster.meshesCreated, (unsigned long long)raster.materialsCreated);
        text += line;
        std::snprintf(line, sizeof(line),
                      "# raster staging: %llu buffers (%llu newly allocated), allocate+map %.3f ms, write %.3f ms, "
                      "memory flags 0x%x | skinned vertices %llu, skin upload %.3f ms, skinning %.3f ms\n",
                      (unsigned long long)raster.stagingBuffers, (unsigned long long)raster.stagingAllocated,
                      raster.stagingAllocateMilliseconds, raster.stagingWriteMilliseconds, raster.stagingMemoryFlags,
                      (unsigned long long)raster.skinnedVertices, raster.skinUploadMilliseconds,
                      raster.skinningMilliseconds);
        text += line;
        std::snprintf(line, sizeof(line),
                      "# raster retire: recycle %.3f ms (%llu buffers), materials %.3f (%llu), textures %.3f (%llu), "
                      "meshes %.3f (%llu), paths %.3f (%llu)\n",
                      raster.recycleMilliseconds, (unsigned long long)raster.recycledBuffers,
                      raster.retireMaterialsMilliseconds, (unsigned long long)raster.materialCount,
                      raster.retireTexturesMilliseconds, (unsigned long long)raster.textureCount,
                      raster.retireMeshesMilliseconds, (unsigned long long)raster.meshCount,
                      raster.retirePathsMilliseconds, (unsigned long long)raster.pathCount);
        text += line;
        if (g_runtime.latencyRequested()) {
          std::snprintf(line, sizeof(line), "# low latency: %s\n",
                        g_runtime.latencyActive() ? "on" : "off (raster backend)");
          text += line;
        }
        if (const auto caller = g_runtime.asyncCallerStatistics(); caller.active) {
          std::snprintf(line, sizeof(line),
                        "# asynchronous present, caller: call %.3f ms (wait for render thread %.3f, setup %.3f, capture "
                        "%.3f, ui %.3f)\n",
                        caller.callMilliseconds, caller.waitMilliseconds, caller.setupMilliseconds,
                        caller.captureMilliseconds, caller.uiMilliseconds);
          text += line;
        }
      }
    }
    std::ofstream(directory / "timings.txt") << text;
  }
}
} // namespace

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_Present(const remixapi_PresentInfo *info) {
  try {
    serviceDevRequests();
    PFN_remixapi_BridgeCallback presentCallback = nullptr;
    remixapi_ErrorCode status = REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    {
      std::lock_guard lock(g_apiMutex);
      status = g_runtime.present(info);
#if REMIX_HAS_USD
      if (status == REMIXAPI_ERROR_CODE_SUCCESS)
        remix::usd::modBoundary(g_runtime.raster());
#endif
#if !defined(_WIN32)
      // Validation and reconstruction SDKs initialize process-static resources on
      // first use. Async preparation can activate them after the initial raster
      // frame. Renew cleanup after each renderer's first use so our GPU wait and
      // resource release precede the SDK's process-exit destructors. The mask
      // bounds registrations per startup, independently of frame count/resizes.
      uint32_t exitStages = g_runtime.hasStartedFrame() ? 1u : 0u;
      if (g_runtime.backendCapabilities().activeBackend == 1)
        exitStages |= 2u;
      const auto reconstruction = g_runtime.reconstructionStatistics();
      if (reconstruction.frames)
        exitStages |= 4u << uint32_t(reconstruction.backend);
      if (exitStages & ~g_frameExitStages) {
        if (std::atexit([] { remixapi_Shutdown(); }) != 0) {
          g_runtime.shutdown();
          return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
        }
        g_frameExitStages |= exitStages;
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
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}

REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterCallbacks(
    PFN_remixapi_BridgeCallback beginSceneCallback, PFN_remixapi_BridgeCallback endSceneCallback,
    PFN_remixapi_BridgeCallback presentCallback) {
  try {
    std::lock_guard lock(g_apiMutex);
    g_beginSceneCallback = beginSceneCallback;
    g_endSceneCallback = endSceneCallback;
    g_presentCallback = presentCallback;
    return REMIXAPI_ERROR_CODE_SUCCESS;
  } catch (...) {
    return REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
  }
}
}
