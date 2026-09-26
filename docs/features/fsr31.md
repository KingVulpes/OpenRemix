# FSR 3.1 upscaling

F12 → **Rendering → Reconstruction → Requested upscaler → FSR 3.1** enables AMD FidelityFX Super Resolution 3.1.4 for
the path-traced world. The default `auto` selects DLSS + Ray Reconstruction, DLSS SR, then native rendering according to availability.
FSR replaces TAA while active. REBLUR SH, local-light REBLUR, SIGMA and SHARC remain
independent lighting controls. Frame generation is not included.

| Preset | Per-axis ratio | Render size at 1920×1080 |
| --- | --- | --- |
| Native AA | 1 | 1920×1080 |
| Quality | 1.5 | 1280×720 |
| Balanced | 1.7 | 1129×635 |
| Performance | 2 | 960×540 |
| Ultra Performance | 3 | 640×360 |

Quality is the initial preset. Optional RCAS sharpening ranges from 0 to 1 and
starts at 0. Lower render resolutions reduce lighting samples and geometric
coverage; FSR reconstructs edges but does not replace a lighting denoiser.

The configuration keys under `rtx.native.pathtrace.` are `upscaler` (`off`, `auto`, `dlss` or
`fsr31`), `fsrQuality` (`native-aa`, `quality`, `balanced`, `performance`, or
`ultra-performance`) and `fsrSharpness` (finite number from 0 to 1). Invalid values
are rejected before changing settings. F12 displays actual render/display sizes,
FSR GPU time, and initialization fallback reasons.

## Frame integration

The world sky, primary G-buffer, ray queries and denoisers use render resolution.
FSR receives linear HDR color after lighting denoising and transparent composition,
before output encoding. Exposure is fixed at 1, matching the renderer's existing
linear-light composition. Minecraft UI, particles and the held item are drawn at
display resolution afterward. A separate, unjittered display-resolution depth pass
keeps their occlusion independent of reconstructed color.

Motion vectors are unjittered current-to-previous displacements in render pixels,
with a dispatch scale of (1,1). Camera history retains the exact submitted
view-projection matrix after the frame is submitted; reconstructing it from a
rounded inverse would create false motion on stationary surfaces away from the
world origin. The official FSR phase-count and Halton helpers
provide subpixel offsets in render pixels. The dispatch negates the renderer's
ray-sampling offset to match FSR's projected-geometry jitter convention.
Device depth is reconstructed from the same primary surface view depth using
a reversed-depth projection, supporting either camera handedness. Converting the
projection before evaluating depth preserves precision across wide capture clip
ranges; subtracting rounded ordinary depth from one would not recover it. FSR's
near/far endpoints are solved directly from the supplied projection, without an
FP32 matrix inverse. Background depth is zero. FSR is enabled only for finite
perspective world cameras.

Transparent radiance follows its opaque background's motion. A preparation pass
writes reactive and transparency/composition masks from accumulated transmission,
including colored glass and water. Opacity reduces history retention without
changing the lighting estimator. Water indirect reuse can use either TAA or FSR.
World texture sampling currently uses explicit mip level zero, so no additional
mip bias applies to those samples.

FSR owns separate history and resets for target/preset recreation and the existing
camera/world reset signal. Ordinary movement and illumination changes keep history.
NRD retains its existing lighting-confidence response. Progressive accumulation and
SHARC's debug view temporarily use native resolution and resume FSR afterward.

## Vulkan ownership and portability

`upscale` owns the FSR context, input preparation, shared scratch textures, output,
compositor and timestamps. The path tracer owns the render/display extent split.
Resource replacement occurs after the previous frame fence; external images return
to GENERAL layout after dispatch. Native tracing keeps its original descriptor
layout. FSR context creation is transactional, so a partial allocation or pipeline
failure releases resources before returning to native rendering.

The pinned source is FidelityFX SDK v1.1.4, revision
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`. `third_party/fsr31/upstream.json`
records original hashes and the hashes of explicitly patched files. Portability
patches correct wide-character context capacity, Vulkan scratch alignment, and the
GLSL luma-history storage format. Private bounded-CRT shims and a Vulkan 1.1 core
entry-point adapter keep platform glue out of the public ABI.

The embedded Vulkan permutations use FP32, HDR, unjittered render-resolution motion
and inverted depth. They do not force subgroup size or require FP16 to be enabled
on the logical device. Required formats, subgroup quad operations and unformatted
storage writes are checked before context creation. Public SDK structures and
exports are unchanged.

Sources: [AMD's upscaler integration guide](https://gpuopen.com/manuals/fidelityfx_sdk/techniques/super-resolution-upscaler/)
and [FidelityFX SDK v1.1.4](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/tree/v1.1.4).
