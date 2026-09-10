#include "scene.h"
#include <remix/remix_c.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#if defined(_WIN32)
#include <string>
#endif

int main(int argc, char **argv) {
#if defined(_WIN32)
  std::wstring path = L"remix.dll";
  if (argc > 1) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, nullptr, 0);
    if (!count) {
      return 1;
    }
    path.resize(count);
    MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, path.data(), count);
  }
  const auto libraryPath = path.c_str();
#elif defined(__APPLE__)
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.0.dylib";
#elif defined(__ANDROID__)
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.so";
#else
  const char *libraryPath = argc > 1 ? argv[1] : "libremix.so.0";
#endif
  const unsigned maxFrames = argc > 2 ? static_cast<unsigned>(std::strtoul(argv[2], nullptr, 10)) : 0;
  remixapi_Interface api = {};
  remixapi_HMODULE library = nullptr;
  auto status = remixapi_lib_loadRemixDllAndInitialize(libraryPath, &api, &library);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "Remix load failed: 0x%x\n", unsigned(status));
    return 1;
  }
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowTitle = "Remix Raster";
#endif
  status = api.Startup(&startup);
  if (status == REMIXAPI_ERROR_CODE_SUCCESS && argc > 3) {
    auto selected = api.SetConfigVariable("rtx.native.backend", argv[3]);
    if (selected != REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE) status = selected;
  }
  SampleScene scene;
  if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
    status = scene.create(api);
  }
  if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
    status = api.SetUIState(REMIXAPI_UI_STATE_ADVANCED);
  }
  const auto start = std::chrono::steady_clock::now();
  for (unsigned frame = 0; status == REMIXAPI_ERROR_CODE_SUCCESS && (!maxFrames || frame < maxFrames);
       ++frame) {
    status = api.PumpEvents();
    if (status != REMIXAPI_ERROR_CODE_SUCCESS) {
      break;
    }
    remixapi_WindowState state = {};
    status = api.GetWindowState(&state);
    if (status != REMIXAPI_ERROR_CODE_SUCCESS || state.closeRequested) {
      break;
    }
    status = scene.draw(api, std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count(),
                        state.drawableWidth, state.drawableHeight);
    if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
      status = api.Present(nullptr);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(state.drawableWidth ? 1 : 50));
  }
  const auto shutdown = remixapi_lib_shutdownAndUnloadRemixDll(&api, library);
  if (status != REMIXAPI_ERROR_CODE_SUCCESS || shutdown != REMIXAPI_ERROR_CODE_SUCCESS) {
    std::fprintf(stderr, "Remix runtime failed: 0x%x, shutdown: 0x%x\n", unsigned(status),
                 unsigned(shutdown));
    return 1;
  }
  return 0;
}
