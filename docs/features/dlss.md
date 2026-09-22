# DLSS Super Resolution and Ray Reconstruction

Native Vulkan NGX support is enabled by default on Linux x64 and Windows x64
clang-cl builds. The pinned SDK is 310.9.1, revision
`374959484e79a640feaba44c93ac8cfb0a03f5b5`. Set Meson's `-Ddlss=false` to exclude
NGX. Other targets retain native and FSR rendering.

`rtx.native.pathtrace.upscaler=auto` defaults to DLSS with Ray Reconstruction,
then DLSS Super Resolution, then native rendering according to feature availability.
`dlss` uses the same capability fallbacks. `off` and `fsr31` remain selectable.
`dlssRayReconstruction=1` is the default; `0` selects SR with the existing denoisers.
`dlssQuality` accepts `native-aa` (DLAA), `quality` (default), `balanced`,
`performance`, and `ultra-performance`. Both features use SDK-reported dimensions.
FSR retains its separate quality/sharpening controls. Frame generation is not included.

F12 shows the requested and active backends, actual render/display sizes, GPU
reconstruction time and fallback reason. RR-managed controls retain their saved
values. Progressive accumulation and SHARC's reference/debug view bypass
reconstruction and return to native resolution.

## Frame contract

SR consumes denoised linear HDR color, standard hardware depth and unjittered
current-to-previous motion in render pixels. REBLUR, local-light denoising and
SIGMA continue to operate normally. Both SR and RR replace TAA.

RR consumes the current unfiltered full lighting result, including sunlight,
local lights, visible emissives, indirect transport, sky and transparent sheets.
It performs reconstruction and upscaling in one evaluation. NRD filters and TAA
are bypassed; their shared temporal guide allocation/preparation remains available.
The raw color never passes through NRD's biased material demodulation.

Dedicated RR textures contain linear diffuse reflectance, average GGX specular
reflectivity using the SDK appendix approximation, world-space shading normals
with linear roughness in alpha, and the unnormalized first transport hit distance.
Material normal maps are included. Distances never come from local shadow samples.
Sky guides use diffuse 0.5, specular/normal/roughness zero and distance 65504.
Transparent albedos blend with coverage and colored transmission using the same
composition as radiance. Depth, normals, motion and hit distance consistently
follow the opaque background. Highly reflective transparent surfaces may need
additional specular motion guides in future work.

The shared preparation pass converts positive view depth to ordinary hardware
depth using the current projection and converts four-channel temporal motion to
RG32F. Sky depth is 1. Matrices are unjittered: GLSL column-vector matrices are
byte-identical to the transposed row-vector/row-major matrices NGX expects.
Halton jitter is negated to match NGX's projected-geometry convention. A 1x1 R32F
texture explicitly supplies exposure 1; pre-exposure and exposure scale are 1.
Camera/world resets, resolution and mode changes reset reconstruction history.

RR derives independent frame settings: no blue noise, repeated shadow phases,
water/PBR screen-space indirect reuse, or checkerboard family selection. Shared
family-budget mode uses combined sampling; per-family budgets still sample both
families. Configured sample counts are preserved. LightBVH importance sampling,
fresh visibility rays and SHARC remain active. Extra rays from disabling reuse
must be included in performance comparisons.

World reconstruction precedes display-resolution particles, held items and UI.
The existing unjittered display-resolution depth pass preserves their occlusion.

## Ownership and failure handling

The device-lifetime NGX context discovers instance/device requirements for SR and
RR independently, before Vulkan device creation. Missing optional support does
not disable ordinary rendering. Cache files use SDL's writable application-data
path. Release libraries and notices live in `dlss/310.9.1` beside the renderer;
Meson builds use an isolated adjacent `upscale` directory. Installed older NGX
feature libraries are not used as a substitute for missing packaged files.

Feature creation is transactional. Resolution and backend changes wait for all
outstanding device work before destroying features, images and histories.
Input layouts transition from GENERAL to SHADER_READ_ONLY_OPTIMAL for NGX and
return to GENERAL. Output remains GENERAL. Evaluation failure spatially scales
the current frame, latches the failed backend, and selects the next fallback at
the next safe frame boundary. Failed backends are retried only after an explicit
mode/quality/RR-setting change; NGX initialization is attempted once per device.

SR model K and RR model E are selected across quality modes. SDK 310.9.1's default
SR L/M and RR F models produced internal Vulkan validation errors on the tested
Linux driver. Native Windows RTX testing remains a separate release gate.

`remix_reconstruction_ext.h` adds an optional size-versioned diagnostics export.
It reports capabilities, requested/active modes, resolutions, evaluations,
resets, failures, allocation bytes and GPU time. The retained API table and
existing public layouts remain unchanged.

Sources: [NVIDIA SDK](https://github.com/NVIDIA/DLSS/tree/374959484e79a640feaba44c93ac8cfb0a03f5b5),
[RR integration guide](https://github.com/NVIDIA/DLSS/blob/374959484e79a640feaba44c93ac8cfb0a03f5b5/doc/DLSS-RR%20Integration%20Guide.pdf).
