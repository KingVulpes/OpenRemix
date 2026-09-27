# Water indirect sampling and ray traversal

Indirect lighting retains its diffuse estimator, bounce limit, alpha cutouts,
tinted transmission and SHARC lookup rules. Candidate intersections evaluate
opacity without the RGB material combiner or vertex-color transfer function.
Always-rejected alpha comparisons avoid texture access. Always-accepted
comparisons also avoid it unless coverage blending needs to reject empty texels.
Tagged paint layers resolve into opaque receiver materials before lighting and
do not act as independent shadow blockers; see [USD integration](usd-toolkit.md).
Motion vertices are reconstructed for primary guides only; continuation and SHARC
update hits do not need them.

Double-sided, non-blended, non-transmitting surfaces whose alpha test always
passes use opaque hardware traversal. The CPU proof includes the alpha combiner,
vertex alpha, tint and mip-zero atlas coverage for each triangle, with linear
filter neighbors where needed. Uncertain coverage, atlas boundaries, wrapped UVs
and one-sided surfaces retain shader candidate checks. Cached proofs follow
immutable material, texture and mesh snapshots, including sprite animation.
BLAS identity includes each surface's opacity so instances sharing geometry can
use different alpha states. This changes no rays, light budgets, PDFs or denoising.
For profiling, `REMIX_OPACITY_DIAGNOSTICS=1` logs opaque and candidate surface and
triangle counts during scene preparation; leave it unset for timing captures.

Sun visibility carries blocker distance and transmission instead of the full
diffuse hit record. SIGMA queries retain the nearest blocker and first attenuating
sheet for penumbra reconstruction. Ordinary secondary sun shadows can terminate
at any accepted opaque blocker. Zero sun radiance skips shadow queries even when
local lights are disabled.

## Translucent surface lighting

Single visible alpha-blended surfaces use a separate shading dispatch.
Overlapping layers use bounded front-to-back shading, retaining only the current
sheet across continuation rays instead of an eight-sheet array. These paths
preserve the existing alpha/tinted transmission model; they do not introduce
dielectric refraction or change water opacity.

`rtx.native.pathtrace.translucencyDenoiser` defaults to `true`. A short visibility
pass captures the foreground normal/roughness, depth, motion, material factors,
and history validity before opaque shading reuses the raster scratch images.
Foreground diffuse/specular lighting has its own REBLUR instance and histories.
Filtering precedes alpha/tint composition; emission remains unfiltered. Pixels
with multiple visible sheets invalidate this foreground history and use the
existing layered composition. Removing a surface or resetting the world cannot
leave its lighting in the output. Disabling this setting preserves opaque history.
Direct-light and continuation distances accompany the lighting estimates. A small
foreground prefilter reduces temporal clipping of bright light-selection samples.

This filter runs with NRD and before DLSS Ray Reconstruction when NRD is available.
For RR, the combined input contains filtered foreground light and the original
transmitted content. A single sheet with opacity at least 0.5 supplies the depth,
motion, normal/roughness, and specular distance guides; lighter sheets retain
background geometry guides. This dominant-layer policy is an approximation for
mixed pixels, not a separate RR reconstruction of every layer. Raster feedback
depth remains independent. Raw/progressive rendering bypasses this filter.

`rtx.native.pathtrace.translucencyReuse` defaults to `false`. Its performance
benefit depends on the scene and reconstruction mode, so it is opt-in. With
ordinary NRD, opaque filtering completes before translucent continuation tracing. At an opaque
endpoint, the single-sheet tracer can reuse this frame's filtered radiance after
checking screen bounds, depth, normal, roughness, material family, and material
factors. Rough PBR dielectrics additionally require closely matching view
directions. Glossy/metallic surfaces, missing matches, off-screen hits, RR, raw
references, and indirect clamping retain normal tracing and SHARC fallback.
Reuse is a filtered path-tail approximation, like radiance caching; it is not an
unbiased fixed-bounce estimator. It adds no scene rays.

Both settings are exposed in F12 and save requested values in profiles. Profiles
omitting them enable surface denoising and leave opaque reuse disabled. Pipelines
use the device-owned cache; foreground descriptors/constants follow completed frame slots, while images and
history remain ordered on the graphics queue. Foreground filtering time and
memory are included in the existing denoising statistics.

## Water indirect sample sharing

`rtx.native.pathtrace.waterIndirectReuse` and the default-enabled F12 **Reuse water
indirect samples** checkbox enable a small spatial approximation for diffuse irradiance.
It activates only for non-PBR sheets with TAA or an SR upscaler, one indirect sample, nonzero bounces, and
without progressive accumulation. Raw, NRD-only and multi-sample reference modes
retain independent sampling at every surface pixel.

Within each 2×2 pixel patch, reuse requires exactly one visible blended
sheet at all four pixels. Normals must agree, points must lie on the same plane
within 0.0001 world units, and each point must be within 0.25 world units of the
patch origin. Failed checks retain independent samples. This conservatively
excludes shorelines, overlapping layers, discontinuous surfaces and distant
patches with large projected footprints.

Two pixels on alternating diagonals trace diffuse continuations and retain their
own irradiance; the two missing pixels reconstruct it from their mean. This halves
the indirect ray count inside a compatible patch. Each receiver applies its own
albedo, opacity and tint. Shared samples carry their continuation distance into
the foreground denoiser; a valid dark estimate is distinct from a missing sample.
Direct sun/torch lighting, opaque background lighting and coverage remain at full
resolution. The existing TAA resolves the final composition; its motion, clipping
and history rules are unchanged. Irradiance variation within a compatible patch
is smoothed, so this mode is not an unbiased per-pixel reference estimator. Sparse
sampling can increase temporal noise near indirect occlusion boundaries; disable
the checkbox for independent sampling at every pixel.

Two workgroup barriers publish geometry and irradiance in the single-sheet
transparency dispatch. Partial workgroups participate before returning; this
sample-sharing feature needs no additional image, frame pass, persistent history,
public ABI field or device feature. The G-buffer's private normal tag distinguishes blended
sheets, and a reserved private frame word carries the setting. Setting changes
invalidate cached output without a global temporal-history reset.

## References

The local *Ray Tracing Gems II*, chapter 14, discusses estimator weighting and the
noise introduced by low sampling probabilities; this implementation reconstructs
a sparse diffuse signal instead of intermittently dropping it. NRD-Sample's
`TraceOpaque.cs.hlsl` provides a reference for alternating sample locations and
signal reconstruction. Its `TraceTransparent.cs.hlsl` keeps transparent transport
separate and reuses lighting with explicit validity checks. The present renderer
retains its existing alpha-composited Lambert/PBR surface model, without introducing the sample's
specular/refraction or screen-history lighting model. Ray offsets retain the
*Ray Tracing Gems I*, chapter 6, construction.
