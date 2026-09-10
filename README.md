# Remix native runtime

Remix is a portable SDL3/Vulkan unlit raster renderer using the retained Remix 0.1003
API. Raster is the default. An opt-in Vulkan ray-query backend adds distant-light
shadows and diffuse indirect illumination to textured world geometry, with a rasterized
G-buffer for primary visibility. Both backends
share meshes, materials, textures, skinning, instances, and raster game UI.
The native sample draws rotating textured cubes and a HUD overlay entirely through
the public API. Its Dear ImGui debug panel provides device information, frame timing,
clear-color, VSync and text-input controls; API clients start with the panel hidden.

The former scene renderer, DXVK resource abstractions, Direct3D injection runtime,
32-bit bridge, legacy RTX shaders and upscalers have been removed. The focused
path tracer now includes pinned diffuse REBLUR SH and native-resolution TAA. Advanced material
effects use simple documented fallbacks. See the [raster implementation](docs/raster-renderer-design.md)
and [path-tracing implementation, controls and limits](docs/path-tracing.md).

For source ownership, feature folders, and maintenance commands, see the
[architecture guide](docs/architecture.md). Local build outputs belong under `_work/`.

## Build

Supported targets are Linux x64 (portable packages target glibc 2.28), Windows x64,
macOS arm64 and Android arm64. Use Clang, Meson 1.3+ and Ninja. Dependencies are SDL3
(pinned at 3.4.14), Vulkan headers/loader, and the vendored Dear ImGui v1.92.9b core,
SDL3 backend and Vulkan backend. Pinned stb_image and bcdec headers provide PNG/DDS
decoding without extra runtime libraries. Vulkan-Headers is the only retained submodule.
Raster, tracing and ImGui shaders are embedded; normal builds need no shader compiler.

```sh
git submodule update --init include/vulkan
CC=clang CXX=clang++ meson setup _work/build/native
meson compile -C _work/build/native
meson test -C _work/build/native --print-errorlogs
_work/build/native/samples/remix-native/remix-native-sample "$PWD/_work/build/native/src/native/libremix.so.0" 300
```

This uses installed SDL3. To build the pinned SDL3 fallback, pass
`-Duse_system_sdl=false`. Alternatively, build a shared dependency prefix:

```sh
python3 scripts/common/build-dependencies.py --output _work/deps/linux
scripts/linux/build-remix.sh _work/build/native _work/deps/linux/prefix
```

`-Ddeps_root=/absolute/path/to/prefix` works on every platform. The platform SDK
supplies Vulkan. The raster backend requires Vulkan 1.1, a graphics/present queue and a surface
supporting color attachments and at least two swapchain images. Ray tracing, compute
queues and NVIDIA hardware are not required.

To select tracing after Startup, call
`SetConfigVariable("rtx.native.backend", "pathtrace")`, use the debug panel selector,
or append `pathtrace` after the sample's frame-count argument. Unsupported devices
return `FEATURE_UNAVAILABLE` with a capability reason and retain raster. Tracing
requires Vulkan 1.2, hardware acceleration structures, ray queries, buffer device
addresses, nonuniform texture indexing and a graphics/compute/presentation queue.
Defaults are rasterized primary visibility, native resolution, one sample per pixel, two indirect bounces,
REBLUR SH and TAA. Progressive accumulation bypasses both filters. `-Dnrd=false`
omits NRD while retaining TAA; no external NRD installation is needed.

See [platform builds and packages](docs/platforms.md) and [API behavior](docs/api.md).
The public C/C++ SDK headers and the 0.1003 ABI layouts remain unchanged, including
POSIX library names and the Windows wide-character loader interface.

## Validation

Normal tests exercise resource validation, texture decoding, C ABI, loader,
compatibility exports, failed startup cleanup and SDL text editing.
Enable `-Dgpu_tests=true` for deterministic GPU pixel-readback tests and presentation,
resize, hide/restore, fullscreen, VSync, wrong-thread, callback re-entry and repeated
lifecycle checks. Linux also injects instance/device/framebuffer/submission failures
and device loss. Set `REMIX_VULKAN_VALIDATION=1` with the Khronos validation layer
installed; `VK_LAYER_VALIDATE_SYNC=1` enables synchronization validation in recent layers.
A missing requested validation layer causes startup to fail.

```sh
meson configure _work/build/native -Dgpu_tests=true
SDL_VIDEODRIVER=offscreen REMIX_VULKAN_VALIDATION=1 \
  meson test -C _work/build/native --print-errorlogs --num-processes 1
```

SDL offscreen Vulkan support depends on the installed loader/driver. A desktop session
or Xvfb with a suitable Vulkan driver is another option. See [verification results](docs/verification.md)
for the tested scope and the remaining device checks. The latest structural refactor
is covered in [cleanup verification](docs/cleanup-verification.md).
REBLUR SH/TAA quality, profiling, platform coverage and rollout are recorded in
[denoising verification](docs/denoising-verification.md).

## Optional driver tooling

`-Dvulkan_probes=true` builds the standalone ray-query, ray-pipeline, bindless and
binary semaphore probes in `tools/vulkan-probes`. Building probes requires glslangValidator
and spirv-val. They run only with `-Dgpu_tests=true`; they are independent of the renderer.
`python3 scripts/shaders/generate.py raster` regenerates embedded raster shaders
using those same tools; `--validate` checks the existing SPIR-V with just spirv-val.
`scripts/shaders/generate.py pathtrace` provides the same commands for the tracing shaders;
`--check` with either backend verifies that the checked-in binary matches its source.
The `denoise` and `nrd` targets cover temporal composition and pinned NRD shaders;
see [the optional DXC toolchain](docs/shader-toolchain.md) for regeneration.
Pinned experimental MoltenVK/SPIRV-Cross revisions and patches remain under
`tools/moltenvk`. `build-moltenvk.py` builds that experimental driver separately.
Normal macOS builds/packages use the SDK's MoltenVK.
[Historical driver findings](docs/history/native-macos-blocker.md) describe the removed renderer.

Original project licenses remain in `LICENSE` and `LICENSE-MIT`. Current dependency
notices are in `ThirdPartyLicenses.txt`; removed dependency notices are archived under `docs/history`. ImGui's upstream license is in `third_party/imgui/LICENSE.txt`.
