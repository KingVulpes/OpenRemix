> This project uses AI-generated code
# OpenRemix
﻿
A performance-focused, multi-platform alternative to [dxvk-remix](https://github.com/NVIDIAGameWorks/dxvk-remix). 
﻿
The entire codebase is different, the only Remix-part of OpenRemix is the compatibility with OpenUSD and the Remix API
﻿
## Features
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

## Projects Using OpenRemix
- [BetaRT](https://github.com/sambow23/BetaRT/tree/openremix)
- [garrys-mod-rtx-remixed-2](https://github.com/sambow23/garrys-mod-rtx-remixed-2)
- [UnityRTX](https://github.com/Veitaserum4/UnityRTX/tree/unity-bridge-openremix)

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
