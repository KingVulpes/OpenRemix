# Platform builds and packages

All builds use Meson/Ninja and Clang. Initialize the `include/vulkan` submodule.
SDL3 remains pinned to 3.4.14; `scripts/common/build-dependencies.py` verifies the
archive checksum and builds only SDL3. Dear ImGui v1.92.9b is vendored with checksums.
PNG/DDS decoders and embedded raster shaders are included in the source tree.
They add no runtime library or normal-build shader compiler dependency.
Use fresh build directories when switching from the removed renderer.

## Linux x64 / glibc 2.28

Use Rocky Linux 8 (or a compatible sysroot) for portable binaries. Building on a newer
distribution does not establish a glibc 2.28 baseline. The CI job builds and packages
inside Rocky Linux 8 and rejects runtime, SDL3 or sample binaries requiring newer glibc.
Clang, Python 3.9+, Meson, CMake, Ninja, Vulkan loader development files and SDL platform
headers are required. The system supplies libvulkan and the GPU driver.

```sh
python3 scripts/common/build-dependencies.py --output _work/deps/linux
scripts/linux/build-remix.sh _work/build/linux _work/deps/linux/prefix
python3 scripts/common/package-remix.py --build _work/build/linux --deps _work/deps/linux --output _work/packages/linux
_work/packages/linux/bin/run-sample 300
```

The package includes the versioned `libremix.so.0.1003.0`, `.so.0` and `.so` aliases,
SDL3, the sample, SDK headers, pin and license notices. No Vulkan driver is bundled on Linux.

## Windows x64

### Cross-build from Linux

Linux hosts can use Clang with the x86_64 MinGW-w64 headers, libraries and runtime,
plus Python, CMake, Meson, Ninja, pkg-config, LLVM binutils and Wine:

Arch Linux specific packages: `sudo pacman -S --needed clang llvm mingw-w64-gcc meson ninja cmake pkgconf python wine git`

```sh
python3 scripts/linux/build-windows-remix.py
python3 scripts/common/package-remix.py --target windows --build _work/build/windows-cross \
  --deps _work/deps/windows --output _work/packages/windows-cross
```

Use `--cmake /path/to/cmake` if CMake is outside PATH. The cross-build entry point
builds the pinned SDL3, generates a Windows import library from the verified Vulkan
1.4.309 export definition, and with `--tests` runs CPU tests in an isolated Wine
prefix under `_work/deps/windows`. Windows or Proton supplies the Vulkan loader and GPU driver.
The cross-built package includes the MinGW runtime DLLs alongside SDL3 and remix.dll.

### Build on Windows

Install the Vulkan SDK and Clang, and run in a Visual Studio x64 developer PowerShell.
The compiler is clang-cl with the Visual Studio headers, import libraries and CRT.
Install `meson`, `ninja` and `cmake` through Python or your tool installer.

```powershell
./scripts/windows/build-remix.ps1
python scripts/common/package-remix.py --build _work/build/windows --deps _work/deps/windows --output _work/packages/windows
./_work/packages/windows/bin/remix-native-sample.exe
```

The package contains `remix.dll`, `SDL3.dll` and the sample together in `bin`.
The Vulkan loader/driver and Visual C++ runtime are platform prerequisites. Null HWND
startup creates a 1280×720 window. Existing HWNDs retain native ownership after Shutdown.
GPU tests (`meson configure _work/build/windows -Dgpu_tests=true`) include owned/borrowed HWNDs
and rejection of alternate presentation targets. Hosted Windows CI builds and runs CPU
checks; interactive Vulkan/borrowed-window checks require a suitable Windows machine.

## macOS arm64

Use Apple Clang, Meson, Ninja, CMake and the standard Vulkan SDK. Pass its macOS directory
(the one containing `bin`, `lib`, `share`) as `--vulkan-sdk`.

```sh
python3 scripts/common/build-dependencies.py --output _work/deps/macos
python3 scripts/macos/build-remix.py --vulkan-sdk "$VULKAN_SDK"
python3 scripts/common/package-remix.py --build _work/build/macos --deps _work/deps/macos \
  --vulkan-sdk "$VULKAN_SDK" --output _work/packages/macos
_work/packages/macos/bin/run-sample 300
```

Packaging uses SDK `libMoltenVK.dylib` and `libvulkan.1.dylib`, rewrites local library
references, signs copied libraries ad hoc, and installs a relative ICD manifest. The
runtime names remain `libremix.0.dylib` and `libremix.dylib`. Startup runs on the main
thread. The deployment target for authored code is macOS 13; the selected SDK's binary
minimum OS requirement also applies.

The separately pinned experimental driver, SPIRV-Cross revisions and patches remain
in `tools/moltenvk/dependencies.lock.json`, `build-moltenvk.py` and `patches/`.
`tools/vulkan-probes/macos-run-probe.py` requires explicit experimental GPU opt-in because of the historical
renderer/driver findings; normal shell build and run paths use SDK MoltenVK.

## Android arm64

Use Android NDK r29, SDK platform 35, build-tools 35.0.0, Java and Python. The minimum
API level is 29. Cross-built CPU/ABI tests are compiled; execute device tests on Android.

```sh
python3 scripts/common/build-dependencies.py --ndk /path/to/android-ndk-r29 --output _work/deps/android
python3 scripts/android/build-remix.py --ndk /path/to/android-ndk-r29
python3 scripts/android/build-apk.py --ndk /path/to/android-ndk-r29 --sdk /path/to/android-sdk
python3 scripts/android/run-sample.py --frames 5000 --test-lifecycle
```

`--validation-layer /path/to/libVkLayer_khronos_validation.so` on `build-apk.py` packages
an arm64 validation layer; `run-sample.py --validation` requests it. The APK contains
only `libmain.so`, `libremix.so`, `libSDL3.so`, optional validation, and SDL/Remix Java
activity classes. The sample always enables the debug panel and writes no renderer
presets. SDL forwards touch and text input to the panel. Background/resume recreates
stale Android Vulkan surfaces before presenting again.

Packages use fresh output directories under `_work/packages/`. Build and dependency
outputs belong under `_work/build/` and `_work/deps/`; `ref/` and archived local
artifacts are not package inputs.
