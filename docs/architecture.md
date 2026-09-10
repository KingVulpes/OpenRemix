# Source organization and maintenance

Remix retains the 0.1003 public SDK and exposes two native rendering backends. Raster
is the default; tracing shares validated scene resources and composes with raster
world effects and UI. This document maps implementation ownership; behavioral
contracts remain in [API behavior](api.md), [raster](raster-renderer-design.md), and
[path tracing](path-tracing.md).

## Feature ownership

First-party runtime code lives under `src/native`. Each feature keeps its implementation
and private headers together. Tests mirror these features under `tests`.

| Folder | Owns |
| --- | --- |
| `api` | SDK interface assembly, exported functions, API serialization, callbacks, game values, and compatibility stubs |
| `runtime` | Startup/shutdown, Vulkan device and swapchain lifecycle, frame presentation, backend selection, and screenshots |
| `scene` | CPU resource validation and lifetime, types/math, texture decoding, materials, meshes, cameras, instances, lights, and submitted UI geometry |
| `raster` | GPU uploads and caches, allocation, pipelines, draw preparation, recording, and raster shaders |
| `pathtrace` | Acceleration allocation, geometry cache, BLAS builds/compaction, TLAS builds/refits and policy, frame preparation/accumulation, rasterized primary G-buffer/depth passes, optional SHARC cache ownership/update/resolve/query, dispatch, and tracing shaders |
| `denoise` | Independent REBLUR SH / local radiance / SIGMA instances, dispatch binding, texture pools, per-dispatch constants, confidence filtering, SH resolve, sun visibility resolve, TAA and temporal diagnostics |
| `platform` | SDL windows/surfaces, events, input state, and key/button translation |
| `ui` | ImGui integration and the debug panel |

Headers are included by feature, such as `scene/resources.h`. Vendor headers use
Meson's system include directories; first-party files do not traverse vendor trees
with relative includes. Public ABI headers remain in `public/include/remix`.

## Dependencies and ownership

```mermaid
flowchart TD
  api[API dispatch] --> runtime[Runtime and presentation]
  runtime --> platform[SDL platform]
  runtime --> ui[Debug UI]
  runtime --> tracing[Path tracing]
  runtime --> raster[Raster]
  tracing --> denoise[NRD and temporal composition]
  tracing --> raster
  tracing --> scene[CPU scene resources]
  raster --> scene
```

The scene library has no Vulkan or SDL dependency. Raster and tracing are separate
internal static libraries. Tracing uses raster's existing sampled-material interface
to share GPU textures. This organization keeps the existing ownership model:
`RasterRenderer` derives from `NativeResources`, and the runtime owns the raster
renderer and an optional tracer. A backend's private `gpu.h` declares its state;
method definitions live in focused implementation files.

API process state has exactly one definition in `api/state.cpp`. The API mutex
serializes CPU submissions. Only the startup thread touches GPU state. Callback
invocation stays outside the API lock where the existing re-entry contract requires
it. Device loss, process-exit cleanup, and resource teardown order remain centralized
in the runtime lifecycle.

GPU allocations and compaction copies retain their existing fence lifetimes. When
changing tracing code, preserve the order of completed-frame retirement, BLAS work,
TLAS work, ray queries, composition, and raster overlays. Capacity and active instance
count are distinct. Reorganized files do not introduce new ownership boundaries or
synchronization points.

## Builds and generated files

Use `_work/build/<configuration>`, `_work/deps/<platform>`,
`_work/packages/<platform>`, and `_work/reports/<run>` for local output. These are
ignored. Meson's pinned SDL fallback additionally manages its source/cache under
`subprojects`; retain the tracked `SDL3.wrap`. Explicit script output arguments
continue to support external build directories.

Track architecture and feature documentation only. `.gitignore` allows the existing
guides and the `docs/architecture/` and `docs/features/` folders; add any new
standalone architecture/feature guide to that allowlist. License notices stay
tracked. Test results, verification reports, benchmark data, deployment records
and screenshots belong under ignored `_work/reports/<run>`. Legacy reports under
`docs/` remain local and ignored. Tracked guides should describe behavior and
validation procedures without linking to local reports.

Normal platform entry points live under `scripts/<platform>`. Shared dependency
building and packaging live under `scripts/common`. These replace the former
platform forwarding scripts. Build the shared library from `src/native/meson.build`
to preserve its build-relative location and packaged SDK/library names.

Readable shaders and `shaders/embedded.h` live inside each renderer feature. Use:

```sh
python3 scripts/shaders/generate.py all --check
python3 scripts/shaders/generate.py all --validate
```

Omit the flag to regenerate, or select `raster`, `pathtrace`, `denoise` or `nrd`. Regeneration uses
`glslangValidator`, pinned DXC and SPIR-V Tools; see [shader toolchain](shader-toolchain.md). Validation alone uses `spirv-val`. These tools are
optional for normal builds. Do not hand-edit generated shader arrays.

Vulkan probes and their shaders/platform runners are in `tools/vulkan-probes`.
Experimental driver pins, patches, and builds are in `tools/moltenvk`; the macOS
probe accepts separate `--deps` (SDL3) and `--driver` (experimental MoltenVK) outputs.
Normal macOS packages continue to use SDK MoltenVK.

## Making changes

Put code and tests under the feature that owns the behavior. Add explicit source
entries to that feature's Meson file. Avoid catch-all utility modules and dependencies
from scene code back into a GPU backend. Preserve public structures, calling
conventions, exports, and compatibility stubs; private exports/dispatch declarations
belong under `api`, not in the public SDK.

Use two-space indentation and attached braces. The repository `.clang-format` applies
to maintained first-party C/C++ code; do not format public SDK baselines, vendor files,
or generated shaders. Run the normal tests after changes and GPU/validation checks
when lifecycle, uploads, synchronization, or rendering changes. Platform commands and
limitations are described in [platforms](platforms.md).

Historical local reports and archived evidence preserve their original measurements and
source-path references. Existing artifacts were archived outside this checkout during
the organization change, with SHA-256 manifests and restoration instructions. Git
history, linked worktrees, `ref/`, and external symlink targets were preserved. Restoring
an old build can require its original absolute paths and source revision; archives
are not active dependency prefixes.
