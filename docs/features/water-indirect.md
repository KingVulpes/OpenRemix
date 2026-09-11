# Water indirect sampling and ray traversal

Indirect lighting retains its diffuse estimator, bounce limit, alpha cutouts,
tinted transmission and SHARC lookup rules. Candidate intersections evaluate
opacity without the RGB material combiner or vertex-color transfer function.
Always-accepted and always-rejected alpha comparisons avoid texture access.
Motion vertices are reconstructed for primary guides only; continuation and SHARC
update hits do not need them.

Sun visibility carries blocker distance and transmission instead of the full
diffuse hit record. SIGMA queries retain the nearest blocker and first attenuating
sheet for penumbra reconstruction. Ordinary secondary sun shadows can terminate
at any accepted opaque blocker. Zero sun radiance skips shadow queries even when
local lights are disabled.

## Water reconstruction

`rtx.native.pathtrace.waterIndirectReuse` and the default-enabled F12 **Reuse water
indirect samples** checkbox enable a small spatial approximation for diffuse irradiance.
It activates only with working TAA, one indirect sample, nonzero bounces, and
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
albedo, opacity and tint.
Direct sun/torch lighting, opaque background lighting and coverage remain at full
resolution. The existing TAA resolves the final composition; its motion, clipping
and history rules are unchanged. Irradiance variation within a compatible patch
is smoothed, so this mode is not an unbiased per-pixel reference estimator. Sparse
sampling can increase temporal noise near indirect occlusion boundaries; disable
the checkbox for independent sampling at every pixel.

Two workgroup barriers publish geometry and irradiance in the existing
transparency dispatch. Partial workgroups participate before returning; no new
image, frame pass, persistent history, public ABI field or device feature is
required. The G-buffer's private normal tag distinguishes blended
sheets, and a reserved private frame word carries the setting. Setting changes
invalidate cached output without a global temporal-history reset.

## References

The local *Ray Tracing Gems II*, chapter 14, discusses estimator weighting and the
noise introduced by low sampling probabilities; this implementation reconstructs
a sparse diffuse signal instead of intermittently dropping it. NRD-Sample's
`TraceOpaque.cs.hlsl` provides a reference for alternating sample locations and
signal reconstruction. Its `TraceTransparent.cs.hlsl` keeps transparent transport
separate and reuses lighting with explicit validity checks. The present renderer
retains its existing Lambertian thin-sheet model, without introducing the sample's
specular/refraction or screen-history lighting model. Ray offsets retain the
*Ray Tracing Gems I*, chapter 6, construction.
