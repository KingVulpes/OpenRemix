> Historical findings from the removed renderer, retained for driver investigations.
> This report describes the historical renderer, not the current shell.

# Experimental native macOS runtime

**Experimental:** the Apple M5 sample completes 300 frames at 1280×720 output
with NRD and TAAU, including a scene capture and clean shutdown. Vulkan and Metal
validation are enabled. RTXDI is disabled by default on MoltenVK because its
direct-lighting path currently produces zero diffuse radiance. An earlier run
caused a WindowServer watchdog failure and host reboot; successful subsequent
runs do not establish general driver stability. See
[native-macos-blocker.md](native-macos-blocker.md).

This target builds the SDL3 native Remix API as an arm64 macOS dylib. Rendering
still uses Vulkan; a pinned experimental MoltenVK translates it to Metal,
including ray queries and ray tracing pipelines. This is not a direct Metal
renderer or a port of the Windows D3D9 injection bridge.

The initial target is Apple M5. Use macOS 26 or newer, Xcode with the macOS 26
SDK or newer, Python 3.12+, CMake, Meson, Ninja, and the macOS Vulkan SDK
1.4.350.1. The prototype was tested on an M5 with macOS 27 beta
and Xcode beta; other Apple GPU generations and older OS versions are untested.
Standard MoltenVK releases do not provide the experimental ray tracing path.

## Driver patches

The driver build applies two small patches from `scripts-macos/patches`:

- Advertise the ordinary acceleration-structure descriptor limit for
  update-after-bind pipeline layout accounting. AS descriptors themselves
  are not updated after binding. This accommodates the SDK validator's
  accounting of the ordinary AS set alongside large update-after-bind sets.
- Preserve deferred binary semaphore tokens when a submission includes
  `VkTimelineSemaphoreSubmitInfo`. Values associated with binary semaphores
  must be ignored. The unpatched driver hangs on the isolated semaphore test.

## Compatibility checks

The isolated probe runs with Vulkan validation and Metal API validation. It
checks hit/miss readback for ray queries and ray pipelines, BLAS/TLAS buffer
offsets and device addresses, TLAS updates, 65,536-entry descriptor layouts,
nonuniform buffer accesses at indices 0 and 65,535, and binary semaphore
submission with ignored timeline values. It also supports creating a compute
pipeline from a real Remix SPIR-V file and its descriptor reflection manifest.

`probe-result.json` records the host, SDK, and dependency pins after a successful
run. These probes establish specific capabilities; they do not establish
compatibility for every optional Remix rendering feature or for other GPUs.

## Verified behavior and remaining limits

On 2026-09-07, `_run-macos-final-300` completed 300 frames with NRD in 8.07
seconds; `_run-macos-final-raw-300` completed 300 frames without NRD in 6.02
seconds. Both runs produced lit geometry and shadows, saved 1280×720 captures,
and reported no Vulkan validation errors. These are whole-process wall times,
including startup and capture, not steady-state GPU benchmarks. TAAU uses a
640×360 internal extent with the default resolution scale. Ten CPU/ABI tests
and nine isolated Vulkan/SPIR-V probes passed.

MoltenVK initialization defaults to direct light sampling without RTXDI.
`DXVK_USE_RTXDI=1` explicitly restores the unvalidated RTXDI path for debugging.
The sample's opaque material now explicitly uses alpha-test Always: the previous
zero-initialized field meant Never and discarded every surface. Temporary
G-buffer diagnostics have been removed.

Full TraceRay G-buffer mode exceeded a 40-second test timeout; the verified
renderer uses compute ray queries. The isolated ray-pipeline probe passes, which
does not establish that every full Remix ray-pipeline variant works. Interactive
resize/minimize, other games, other Apple GPUs, and a stable macOS release remain
untested. The Windows bridge, NGX, and proprietary vendor integrations are not
provided by this native target.

