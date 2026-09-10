> Historical findings from the removed renderer, retained for driver investigations.
> This report describes the historical renderer, not the current shell.

# Native Android bring-up

The full arm64 RTX renderer cross-builds with NDK r29 and runs in an SDL3
Activity APK. This is an experimental standalone native API sample, not an
Android BetaRT/game integration. The bootstrap and headless GPU probes remain
available independently.

## Device results (2026-09-07)

Tested through authorized ADB on Galaxy S25 SM-S931U, SM8750, Adreno 830,
Android 16, stock Qualcomm Vulkan 1.3.284. No root, Turnip installation, system
driver replacement, or global graphics setting changes were needed.

- BLAS/TLAS build, ray-query hit/miss and TLAS update readback: pass.
- Bindless ray query with 65,536 descriptors and nonuniform first/last entry
  access: pass. Reported update-after-bind per-stage buffer/image/sampler limits
  are 16,777,216 each.
- Ray-tracing pipelines: unavailable (`VK_KHR_ray_tracing_pipeline` absent).
- Full renderer ABI/CPU tests: 10/10 pass on the phone. SDL text-input testing
  requires Activity/JNI and is explicitly skipped by the shell test runner.
- Packed constant conversion: all 497 generated RTX shader variants pass
  `spirv-val --target-env vulkan1.2 --scalar-block-layout` using the actual
  renderer conversion code. macOS build and 11 CPU/ABI regression tests pass.
- The mobile query preset completed 600 frames, survived background/resume,
  recreated its surface and shut down cleanly with no Vulkan validation errors.
  Its captured frame contains the blue sample triangle. General desktop defaults
  hit device loss; this reduced scene does not establish game compatibility or
  a general performance baseline. This validation-enabled run took about nine
  minutes including cold shader compilation.
- A separate 180-frame run with validation disabled completed cleanly. Touch
  injection opened and closed the Remix menu; screenshots were inspected.

First-use driver shader compilation can take 1–2 minutes. The initial sample
uses one-third resolution, TAAU, compute ray queries and NRD. Clouds, volumetric
fog, RTXDI, NEE cache and primary-surface replacement are disabled. Secondary
bounces remain enabled. DLSS/NGX, frame generation, CUDA, RTXIO and USD asset
replacement are not provided by this Android target.

## Runtime portability

The ray-tracing pipeline extension is optional. G-buffer, direct and indirect
passes select compute ray queries when the feature is absent; pipeline prewarming
follows the same capability check. The volume pass already uses compute queries. Barrier stages and bindless
layouts omit unsupported ray-tracing stages.

The stock driver supports 8/16-bit storage-buffer access but not 8/16-bit
uniform-buffer access. ShaderManager converts RTX constant-buffer SPIR-V
pointers and descriptors to storage buffers on such devices, preserving member
offsets, array strides and push constants. Constant buffers also receive
storage usage and shader-read access. NRD's ordinary 32-bit UBOs stay unchanged.
The generated host shader headers are reused without copying host libraries.

Android swapchains select a supported alpha mode, use compatible render-pass
dependencies and bind presentation semaphores by acquired image. Resume creates
a fresh SDL Vulkan surface. Already-completed presentation errors are checked
as well as pending submissions, so an old acquisition cannot silently be reused.

## Turnip audit

Turnip source was checked again on 2026-09-07, including current upstream
`acbb4f6b8de2768b5ea7d6a83a8e52a263eac5cc` and the Adreno Gen8 fork
`9c475fc367a7283a7eee58501fb48149780f2c1e`. Both extension tables expose
acceleration structures, ray queries, and ray-tracing maintenance1 conditionally,
but neither advertises `VK_KHR_ray_tracing_pipeline`.

Sources: [upstream extension table](https://chromium.googlesource.com/external/gitlab.freedesktop.org/mesa/mesa/+/acbb4f6b8de2768b5ea7d6a83a8e52a263eac5cc/src/freedreno/vulkan/tu_device.cc),
[Gen8 extension table](https://github.com/whitebelyash/mesa-unified/blob/9c475fc367a7283a7eee58501fb48149780f2c1e/src/freedreno/vulkan/tu_device.cc),
and [Mainline Turnip V31 release](https://github.com/whitebelyash/AdrenoToolsDrivers/releases/tag/tu_v31)
(August 7, 2026), which names that Gen8 branch and lists Adreno 830 support.
This was a source/release audit, not a Turnip device run. No Turnip binary has
been installed or benchmarked here. Its performance relative to Qualcomm's
stock driver remains to be measured for the Remix workload. The ray-query-only
renderer path is needed for both reviewed drivers.

