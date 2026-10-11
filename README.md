> This project uses AI-generated code
# OpenRemix
﻿
A performance-focused, multi-platform alternative to [dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix). 
﻿
The entire codebase is different, the only Remix-part of OpenRemix is the compatibility with OpenUSD and the Remix API
﻿
## Features
﻿
- Raster rendering and path tracing, with raster or ray-traced primary visibility, this is used to pass through raster effects/overlays into the Path-traced view 
- Sun, sky, local and emissive lighting, indirect illumination, and soft shadows.
- PBR materials, normal maps, decals, alpha cutouts, and translucent surfaces.
- Mesh instancing, skinning, and animated textures.
- NRD/SIGMA denoising, including separate translucent-surface filtering.
- SHARC radiance caching and progressive accumulation.
- Native TAA, FSR 3.1, DLSS Super Resolution and Ray Reconstruction.
- Autoexposure, threshold-free bloom, and Prism tonemapping.
- Rasterized HUDs, viewmodels, and screen overlays.
- ImGui menu, per-game profiles, GPU diagnostics, and screenshots.
- Background pipeline preparation and persistent shader caching.
- Optional OpenUSD scenes, mod replacements, hot reload, and Linux toolkit viewport.

## Game integration

A game-side integration or bridge supplies scene data through the [Remix SDK](public/include/remix).
Load the library, initialize it and call `Startup`, then create textures, materials,
and meshes. Each frame, submit cameras, lights, instances and game UI, then call
`Present`. Keep event handling, presentation and `Shutdown` on the startup thread.

Raster is the default; select path tracing with
`SetConfigVariable("rtx.native.backend", "pathtrace")`.
Optional, off by default: `rtx.native.textures.keepCompressed = 1` keeps BC1-BC7
textures compressed on the GPU (for textures created afterwards), and
`rtx.native.vramStatistics = 1` makes `GetVramStats` report allocations and the
driver budget. Both are also in the ImGui Rendering tab under Memory.
Also off by default: `rtx.native.memoryPressure = 1` answers `remixapi_GetMemoryPressure`
(Memory), and `rtx.native.meshes.refreshInPlace = 1` honours
`remixapi_MeshInfoRefreshGeometryEXT`, refitting a refreshed mesh's BLAS (Geometry).
`rtx.native.gpuPassTimers = 1` (Diagnostics, GPU pass timings) timestamps every GPU pass
of a frame and lists the costs by size; hosts can log them with `remixapi_GetGpuPassTimingsEXT`.
The `remix_fo4_ext.h` material structs (spec/gloss roughness maps, albedo tint and the
per-instance material override) follow the dxvk-remix fork and are always honoured;
`remixapi_GetSpecGlossMaterialVersion`, `GetMaterialAlbedoTintVersion` and
`GetInstanceMaterialOverrideVersion` return 1 so hosts can drop their CPU fallbacks.
See the [native sample](samples/remix-native).

## Building

Targets: Linux/Windows x64 and macOS/Android arm64. Use Clang, Python 3.9+,
Meson 1.3+, Ninja, CMake, and Vulkan development files. Path tracing requires
hardware ray-query support; DLSS requires compatible NVIDIA hardware.

Linux quick start, building the pinned SDL3 dependency:

```sh
git submodule update --init include/vulkan
CC=clang CXX=clang++ meson setup _work/build/native -Dbuildtype=release -Duse_system_sdl=false
meson compile -C _work/build/native
meson test -C _work/build/native --print-errorlogs
```

On Windows, run `./scripts/windows/build-remix.ps1` from a Visual Studio x64
Developer PowerShell with Clang and the Vulkan SDK installed. See
[platform builds and packaging](docs/platforms.md) for macOS, Android, cross-builds,
and dependency setup. Normal builds use embedded shaders.

## Credits

- NVIDIA: NRD, SIGMA, SHARC and DLSS. AMD: FidelityFX FSR. Pixar and OpenUSD contributors.
- SDL, Vulkan/MoltenVK, Dear ImGui, stb and bcdec contributors.
- TheGreatHmmmmm, Jane, surething on the `RTX Remix Showcase` server for guiding me in the right direction. 

See [third-party notices](ThirdPartyLicenses.txt)
