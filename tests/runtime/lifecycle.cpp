#include "remix-native/scene.h"
#include <SDL3/SDL.h>
#include <remix/remix_c.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace {
  remixapi_Interface api = {};
  int callbacks = 0;
  void require(bool value, const char *message) {
    if (!value) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void checked(remixapi_ErrorCode code, int line) {
    if (code) {
      std::fprintf(stderr, "Unexpected error: 0x%x at line %d (SDL: %s)\n", unsigned(code), line,
                   SDL_GetError());
      std::exit(1);
    }
  }
#define ok(code) checked((code), __LINE__)
  void REMIXAPI_CALL sceneCallback() { require(false, "Shell emitted a scene event"); }
  void REMIXAPI_CALL presentCallback() {
    ++callbacks;
    ok(api.SetGameValue("callback", "reentered"));
    remixapi_FeatureAvailability features = {};
    ok(api.GetFeatureAvailability(&features));
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    opaque.albedoConstant = {1, 1, 1};
    opaque.opacityConstant = 1;
    opaque.alphaTestType = 7;
    remixapi_MaterialInfo material = {};
    material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    material.hash = 0xfeed;
    material.pNext = &opaque;
    remixapi_MaterialHandle mat;
    ok(api.CreateMaterial(&material, &mat));
    remixapi_HardcodedVertex vertices[3] = {};
    vertices[0].position[0] = -1;
    vertices[1].position[1] = 1;
    vertices[2].position[0] = 1;
    for (auto &v : vertices) {
      v.position[2] = 3;
      v.color = 0xffffffff;
    }
    remixapi_MeshInfoSurfaceTriangles surface = {};
    surface.material = mat;
    surface.vertices_values = vertices;
    surface.vertices_count = 3;
    remixapi_MeshInfo meshInfo = {};
    meshInfo.sType = REMIXAPI_STRUCT_TYPE_MESH_INFO;
    meshInfo.hash = 0xbeef;
    meshInfo.surfaces_values = &surface;
    meshInfo.surfaces_count = 1;
    remixapi_MeshHandle mesh;
    ok(api.CreateMeshBatched(&meshInfo, &mesh));
    remixapi_InstanceInfo draw = {};
    draw.sType = REMIXAPI_STRUCT_TYPE_INSTANCE_INFO;
    draw.mesh = mesh;
    draw.doubleSided = 1;
    draw.transform.matrix[0][0] = draw.transform.matrix[1][1] = draw.transform.matrix[2][2] = 1;
    ok(api.DrawInstance(&draw));
    ok(api.DestroyMesh(mesh));
    ok(api.DestroyMaterial(mat));
    // Recursive presentation must not recursively dispatch the callback.
    ok(api.Present(nullptr));
  }
  void stubs(remixapi_ErrorCode expected) {
    remixapi_MaterialInfo material = {};
    material.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO;
    material.hash = 1;
    remixapi_MaterialInfoOpaqueEXT opaque = {};
    opaque.sType = REMIXAPI_STRUCT_TYPE_MATERIAL_INFO_OPAQUE_EXT;
    material.pNext = &opaque;
    remixapi_MaterialHandle handle = reinterpret_cast<remixapi_MaterialHandle>(1);
    const bool initialized = expected != REMIXAPI_ERROR_CODE_NOT_INITIALIZED;
    const auto supported = initialized ? REMIXAPI_ERROR_CODE_SUCCESS : expected;
    require(api.CreateMaterial(&material, &handle) == supported &&
                (initialized ? handle != nullptr : !handle),
            "Material/output");
    remixapi_VramStats stats;
    std::memset(&stats, 0xff, sizeof(stats));
    require(api.GetVramStats(&stats) == expected, "VRAM stub");
    remixapi_VramStats empty = {};
    require(std::memcmp(&stats, &empty, sizeof(stats)) == 0, "VRAM output not cleared");
    require(api.AutoInstancePersistentLights() == supported, "Light stub");
    if (!initialized) {
      require(api.RequestPresentedScreenshot("/tmp/unused.png") == expected, "Screenshot before startup");
    }
    require(api.RequestTextureVramFree() == supported, "Texture VRAM stub");
    require(api.CreateMesh(nullptr, nullptr) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS, "Argument validation");
    uint32_t pixel = 0;
    require(api.RegisterUITexture(1, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, &pixel, 4) == supported,
            "UI texture stub");
    require(api.RegisterUITexture(0, 1, 1, REMIXAPI_FORMAT_R8G8B8A8_UNORM, &pixel, 4) ==
                REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
            "UI texture validation");
    if (initialized) {
      remixapi_LightInfoSphereEXT sphere = {};
      sphere.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO_SPHERE_EXT;
      remixapi_LightInfo light = {};
      light.sType = REMIXAPI_STRUCT_TYPE_LIGHT_INFO;
      light.hash = 0x7788;
      light.pNext = &sphere;
      remixapi_LightHandle dummy = nullptr;
      ok(api.CreateLightBatched(&light, &dummy));
      require(dummy != nullptr, "No-op light creation returned a null handle");
      ok(api.UpdateLightDefinition(dummy, &light));
      ok(api.DrawLightInstance(dummy));
      ok(api.DestroyLight(dummy));
      require(api.DrawLightInstance(dummy) == REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS,
              "Stale light handle accepted");
      ok(api.DestroyLight(dummy));
      ok(api.SetConfigVariable("rtx.someLegacyRendererSetting", "true"));
      remixapi_FogInfo fog = {};
      fog.sType = REMIXAPI_STRUCT_TYPE_FOG_INFO;
      ok(api.SetFogState(&fog));
      remixapi_CameraMediumInfo medium = {};
      medium.sType = REMIXAPI_STRUCT_TYPE_CAMERA_MEDIUM_INFO;
      ok(api.SetCameraMediumMaterial(&medium));
      ok(api.DrawScreenOverlay(nullptr, 0, 0, REMIXAPI_FORMAT_R8G8B8A8_UNORM, 0));
    }
#if defined(_WIN32)
    IDirect3D9Ex *d3d = reinterpret_cast<IDirect3D9Ex *>(1);
    require(api.dxvk_CreateD3D9(0, &d3d) == expected && !d3d, "D3D compatibility stub");
    uint64_t image = 1, done = 1, resume = 1;
    require(api.dxvk_GetExternalSwapchain(&image, &done, &resume) == expected && !image && !done && !resume,
            "External swapchain outputs");
#endif
  }
} // namespace

int main(int argc, char **argv) {
  require(argc >= 2, "Provide runtime library");
  const bool gpu = argc > 2 && std::strcmp(argv[2], "gpu") == 0;
  const bool recover = argc > 2 && std::strcmp(argv[2], "recover") == 0;
  const bool fault = recover || (argc > 2 && std::strcmp(argv[2], "fault") == 0);
  remixapi_HMODULE library = nullptr;
#if defined(_WIN32)
  std::wstring path(argv[1], argv[1] + std::strlen(argv[1]));
  auto libraryPath = path.c_str();
#else
  auto libraryPath = argv[1];
#endif
  ok(remixapi_lib_loadRemixDllAndInitialize(libraryPath, &api, &library));
  auto selectBackend = [&] {
    if (const char *backend = std::getenv("REMIX_TEST_BACKEND")) {
      const auto status = api.SetConfigVariable("rtx.native.backend", backend);
      if (status != REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE)
        ok(status);
    }
  };
  stubs(REMIXAPI_ERROR_CODE_NOT_INITIALIZED);
  remixapi_FeatureAvailability features = {~0u, ~0u, ~0u};
  ok(api.GetFeatureAvailability(&features));
  require(!(features.compiledFeatures & ~REMIXAPI_FEATURE_NRD_BIT) && !features.availableFeatures &&
              !features.dlssFrameGenerationMaxInterpolatedFrames,
          "Removed features advertised");
  ok(api.SetGameValue("plain", "text"));
  uint32_t size = 0;
  char text[10] = {};
  ok(api.GetGameValue("plain", nullptr, 0, &size));
  require(size == 5, "Game value size query");
  ok(api.GetGameValue("plain", text, sizeof(text), &size));
  require(std::strcmp(text, "text") == 0, "Game value round trip");
  ok(api.GetGameValue("missing", text, sizeof(text), &size));
  require(!size && !text[0], "Missing game value safe output");
  remixapi_WindowState state;
  std::memset(&state, 0xff, sizeof(state));
  require(api.GetWindowState(&state) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED && !state.drawableWidth,
          "Window before startup");
  remixapi_StartupInfo startup = {};
  startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
#if !defined(_WIN32)
  startup.windowWidth = 640;
  startup.windowHeight = 480;
#endif
  if (argc > 2 && std::strcmp(argv[2], "exit") == 0) {
    ok(api.Startup(&startup));
    selectBackend();
    SampleScene scene;
    ok(scene.create(api));
    ok(scene.draw(api, 0, 640, 480));
    ok(api.Present(nullptr));
    return 0;
  }
  if (!gpu && !fault) {
    SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy", SDL_HINT_OVERRIDE);
    for (int i = 0; i < 3; ++i) {
      require(api.Startup(&startup) != REMIXAPI_ERROR_CODE_SUCCESS,
              "Dummy driver unexpectedly supports Vulkan");
      require(api.Present(nullptr) == REMIXAPI_ERROR_CODE_NOT_INITIALIZED, "Failed startup retained runtime");
      ok(api.Shutdown());
      ok(api.GetFeatureAvailability(&features));
      require(!features.availableFeatures, "Shutdown retained device feature availability");
    }
  } else if (fault) {
    // Failure injector is Linux-only; an actual startup must work immediately afterward.
    auto status = api.Startup(&startup);
    if (status == REMIXAPI_ERROR_CODE_SUCCESS) {
      selectBackend();
      SampleScene scene;
      ok(scene.create(api));
      ok(scene.draw(api, 0, 640, 480));
      status = api.Present(nullptr);
      if (recover) {
        ok(status);
        ok(api.Present(nullptr));
        ok(api.Present(nullptr));
      } else {
        require(status == REMIXAPI_ERROR_CODE_GENERAL_FAILURE, "Injected frame failure did not propagate");
        require(api.Present(nullptr) == status, "Failed frame resources were reused");
      }
    }
    ok(api.Shutdown());
    ok(api.GetFeatureAvailability(&features));
    require(!features.availableFeatures, "Shutdown retained device feature availability");
    SDL_setenv_unsafe("REMIX_TEST_FAULT", "", 1);
    ok(api.Startup(&startup));
    selectBackend();
    ok(api.Present(nullptr));
    ok(api.Shutdown());
    ok(api.GetFeatureAvailability(&features));
    require(!features.availableFeatures, "Shutdown retained device feature availability");
  } else {
    const bool frameChanges = std::getenv("REMIX_TEST_FRAME_CHANGES") != nullptr;
    for (int cycle = 0; cycle < (frameChanges ? 1 : 3); ++cycle) {
      ok(api.Startup(&startup));
      selectBackend();
      require(api.Startup(&startup) == REMIXAPI_ERROR_CODE_ALREADY_EXISTS, "Duplicate startup");
      require(api.GetUIState() == REMIXAPI_UI_STATE_NONE, "UI initially visible");
      stubs(REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE);
      SampleScene scene;
      ok(scene.create(api));
      std::thread wrongThread([] {
        require(api.Present(nullptr) == REMIXAPI_ERROR_CODE_WRONG_THREAD, "Present thread ownership");
        require(api.PumpEvents() == REMIXAPI_ERROR_CODE_WRONG_THREAD, "Input thread ownership");
        require(api.Shutdown() == REMIXAPI_ERROR_CODE_WRONG_THREAD, "Shutdown thread ownership");
        const uint32_t pixel = 0xffffffff;
        remixapi_TextureInfo texture = {};
        texture.sType = REMIXAPI_STRUCT_TYPE_TEXTURE_INFO;
        texture.hash = 0x777;
        texture.width = texture.height = 1;
        texture.format = REMIXAPI_FORMAT_R8G8B8A8_UNORM;
        texture.data = &pixel;
        texture.dataSize = 4;
        remixapi_TextureHandle handle;
        ok(api.CreateTexture(&texture, &handle));
        ok(api.DestroyTexture(handle));
      });
      wrongThread.join();
      ok(api.SetUIState(REMIXAPI_UI_STATE_ADVANCED));
      ok(api.RegisterCallbacks(sceneCallback, sceneCallback, presentCallback));
      int windowCount = 0;
      auto windows = SDL_GetWindows(&windowCount);
      require(windowCount >= 1, "SDL window missing");
      auto window = windows[0];
      SDL_free(windows);
      for (int frame = 0; frame < 90; ++frame) {
        if (frameChanges) {
          if (frame % 10 == 0)
            std::fprintf(stderr, "Rendering resource replacement: frame %d\n", frame);
          // Resource replacement must also work while the other submission
          // slot still owns descriptors, uploads and readbacks.
          auto config = [&](const char *key, const char *value) {
            ok(api.SetConfigVariable((std::string("rtx.native.pathtrace.") + key).c_str(), value));
          };
          if (frame == 2)
            config("upscaler", "fsr31");
          if (frame == 6)
            config("denoiser", "off");
          if (frame == 12)
            config("denoiser", "reblur-sh");
          if (frame == 16)
            config("sharcCapacityLog2", "16");
          if (frame == 24)
            config("sharcCapacityLog2", "18");
          if (frame == 28)
            config("upscaler", "off");
          if (frame == 34)
            ok(api.SetConfigVariable("rtx.native.backend", "raster"));
          if (frame == 38)
            ok(api.SetConfigVariable("rtx.native.backend", "pathtrace"));
          if (frame == 48)
            config("sigma", "0");
          if (frame == 58)
            config("sigma", "1");
          if (frame == 64)
            config("separateLocalLighting", "1");
          if (frame == 72)
            config("separateLocalLighting", "0");
          if (frame == 80)
            config("upscaler", "fsr31");
        }
        if (frame == 10) {
          require(SDL_SetWindowSize(window, 800, 600), "Resize");
        }
        if (frame == 20) {
          ok(api.SetConfigVariable("rtx.native.vsync", "true"));
        }
        if (frame == 30) {
          ok(api.SetConfigVariable("rtx.native.vsync", "false"));
        }
        if (frame == 40) {
          require(SDL_HideWindow(window), "Hide/minimize");
        }
        if (frame == 45) {
          require(SDL_ShowWindow(window), "Restore");
        }
        if (frame == 50) {
          ok(api.SetFullscreen(1));
        }
        if (frame == 55) {
          ok(api.SetFullscreen(0));
        }
        ok(api.PumpEvents());
        ok(scene.draw(api, float(frame) * .02f, 640, 480));
        ok(api.Present(nullptr));
      }
      require(callbacks == (cycle + 1) * 90, "Present callbacks were lost or recursive");
      ok(api.GetWindowState(&state));
      require(state.drawableWidth && state.drawableHeight, "No drawable presentation");
      ok(api.SetUIState(REMIXAPI_UI_STATE_NONE));
      ok(api.GetWindowState(&state));
      require(!state.guiWantsKeyboard && !state.guiWantsMouse, "Hidden panel captured input");
      ok(api.Shutdown());
      ok(api.GetFeatureAvailability(&features));
      require(!features.availableFeatures, "Shutdown retained device feature availability");
    }
#if defined(_WIN32)
    HWND borrowed = CreateWindowExW(0, L"STATIC", L"Borrowed Remix window", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                                    0, 0, 640, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    require(borrowed != nullptr, "Create borrowed HWND");
    startup.hwnd = borrowed;
    ok(api.Startup(&startup));
    ok(api.Present(nullptr));
    remixapi_PresentInfo present = {};
    present.sType = REMIXAPI_STRUCT_TYPE_PRESENT_INFO;
    present.hwndOverride = borrowed;
    ok(api.Present(&present));
    present.hwndOverride = GetDesktopWindow();
    require(api.Present(&present) == REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE, "Alternate HWND accepted");
    ok(api.Shutdown());
    ok(api.GetFeatureAvailability(&features));
    require(!features.availableFeatures, "Shutdown retained device feature availability");
    require(IsWindow(borrowed), "Shutdown destroyed borrowed HWND");
    DestroyWindow(borrowed);
    startup.forceNoVkSwapchain = 1;
    require(api.Startup(&startup) == REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE,
            "External swapchain startup accepted");
#endif
  }
  if (gpu) {
    startup = {};
    startup.sType = REMIXAPI_STRUCT_TYPE_STARTUP_INFO;
    ok(api.Startup(&startup));
    SampleScene scene;
    ok(scene.create(api));
    ok(scene.draw(api, 0, 640, 480));
    ok(api.Present(nullptr));
    // Unload with a live runtime on the owner thread exercises the destructor path.
#if defined(_WIN32)
    ok(remixapi_lib_shutdownAndUnloadRemixDll(&api, library));
#else
    require(dlclose(library) == 0, "Direct library unload failed");
#endif
    api = {};
  } else {
    ok(remixapi_lib_shutdownAndUnloadRemixDll(&api, library));
  }
  // Verify reloading after destruction of library-static state.
  ok(remixapi_lib_loadRemixDllAndInitialize(libraryPath, &api, &library));
  ok(remixapi_lib_shutdownAndUnloadRemixDll(&api, library));
  return 0;
}
