# SIGMA sun shadows

The renderer uses the pinned NRD 4.18.0 `SIGMA_SHADOW_TRANSLUCENCY` denoiser for
primary opaque sun visibility. REBLUR diffuse SH continues to denoise indirect
lighting, including SHARC results. SIGMA can also run with REBLUR and TAA disabled.

## Signal and penumbra

The distant-light extension's existing `angularDiameterDegrees` field controls
sun size. BetaRT supplies 0.533 degrees, matching the local NRD-Sample default.
Legacy callers that leave the field zero retain hard shadows. The CPU validates
finite diameters in `[0, 180)` before accepting a light definition.

A primary shadow ray samples a uniform disk around the direction toward the sun,
using the tangent of half the angular diameter. Its random stream is independent
of diffuse sampling and confidence retracing. Filtered shadows use an eight-sample
Owen-scrambled Sobol sequence with a 128x128 blue-noise scrambling/ranking tile,
following Heitz et al. and NRD-Sample. The tile repeats in screen space; the
sequence repeats every eight frames to limit shadow animation. A packed immutable
buffer contains the two shadow dimensions and a separate local-light table
(131,232 bytes total). No texture assets,
additional descriptors or additional shadow rays are needed at runtime.

`blueNoiseShadows=0` restores the previous eight hashed sample patterns for
comparison. The blue-noise stream applies only to primary opaque sun shadows
while SIGMA is active. Raw/progressive reference rendering uses the full hashed
frame sequence. Local emitter positions use an independent sampler described in
[Emissive maps](emissive-maps.md). Light selection, indirect directions,
sample counts and bounce limits retain their existing algorithms.

Direct light is split into an analytic, unshadowed Lambertian term evaluated at
the sun's center and sampled RGB visibility. SIGMA receives the blocker distance
and tangent of the angular radius through `SIGMA_FrontEnd_PackPenumbra`. Thus a
nearby blocker has a narrow penumbra and a distant blocker has a wider one.
Back-facing/unlit samples use zero distance; unobstructed samples use the NRD
FP16 maximum sentinel. Finite blocker distances are clamped below that sentinel.

The existing alpha-cutout tests, robust ray offsets and 32-sheet bound apply to
shadow rays. Blended sheets multiply the existing opacity/color transmission.
A ray that escapes after tinted attenuation retains its first attenuating-sheet
distance, allowing SIGMA to filter that colored boundary. An opaque blocker uses
its total distance along the ray. A single effective penumbra is an approximation
when multiple transparent blockers lie at different distances.

`SIGMA_FrontEnd_PackTranslucency` supplies visibility; composition applies
`SIGMA_BackEnd_UnpackShadow` to the output before multiplying the unshadowed direct
term. Albedo and indirect radiance are never inputs to the shadow filter. A zero
sunlight term remains zero regardless of history. Zero indirect bounces still
allows direct sun shadows but disables all indirect illumination.

Opaque depth, world normal and screen motion guides serve both NRD denoisers.
Primary transparent sheets do not have opaque guides, so their own surface
lighting retains the existing center-sun shadow query. Filtered opaque lighting
is still visible through those sheets using the existing tinted composition.
Secondary diffuse hits and SHARC update paths also retain the center-sun direct
estimate. SIGMA does not change sky drawing or add moonlight.

## Ownership and temporal behavior

REBLUR and SIGMA own separate NRD instances, pools, descriptor sets and aligned
constant buffers. Per-dispatch data remains alive until the submitted-frame
fence. Shared opaque guides are read after tracing; memory barriers order NRD
passes and composition. SIGMA adds full-resolution R16F penumbra and RGBA8 input /
output textures; its internal pools follow the pinned SDK description.

The established temporal bindings remain intact. Optional SIGMA images occupy
bindings 26–28, following the existing uniform buffer at binding 25.
The private tracing frame uses its reserved tail address for the sampling tables
and retains its 512-byte layout; the temporal frame remains 432 bytes.
Public SDK layouts and the retained interface table are unchanged.

Camera cuts, world/reset epochs, resize and explicit mode changes retain existing
history behavior. Abrupt sun direction, brightness or diameter changes reset
SIGMA independently. Ordinary time/weather updates keep both denoisers' histories;
REBLUR's existing lighting-confidence pass continues to track indirect changes.
Convergence participates in the existing redraw scheduling. Shadow diagnostic
views bypass final TAA and clear display history when changing views.
Changing the shadow sampler resets only SIGMA history. The immutable sampling
buffer is uploaded once per renderer, with a transfer-to-compute memory barrier;
it survives resize and sampler toggles. Allocation failure keeps SIGMA active
with the previous hashed sampler and avoids retrying the failed allocation.

SIGMA checks its additional image formats and descriptor limits independently.
Unsupported builds/devices or optional shadow initialization failure retain the
existing unfiltered-shadow, REBLUR and TAA path. Progressive accumulation bypasses
SIGMA, REBLUR, TAA and SHARC for reference rendering.

## Controls and diagnostics

All renderer keys use the `rtx.native.pathtrace.` prefix:

| Key | Default | Meaning |
| --- | --- | --- |
| `sigma` | true | Denoise primary opaque sun visibility |
| `blueNoiseShadows` | true | Use the eight-frame blue-noise sun-shadow sequence with SIGMA |
| `sunAngularDiameter` | -1 | Inherit the light's size; override with 0–10 degrees; zero gives hard shadows |
| `shadowDebug` | 0 | 0 final lighting, 1 raw visibility, 2 SIGMA visibility |

The debug panel exposes SIGMA and blue-noise toggles, sun-size override and visibility views.
The optional additive `remixapi_GetShadowStatisticsEXT` reports support, active
state, diameter, history resets and SIGMA GPU time. Existing diffuse NRD timing
continues to measure REBLUR alone. BetaRT forwards `MCRTX_PT_SIGMA`,
`MCRTX_PT_SUN_DIAMETER` and `MCRTX_PT_SHADOW_DEBUG`. Its optional diagnostics append
`sigma`, `gpu_sigma_ms`, `sigma_history_resets` and `sun_diameter_degrees` to CSV;
missing SDK types/exports remain non-fatal and explicitly unavailable. Java/JNI
transport remains protocol v5.

## References and validation

The local [NRD-Sample opaque tracing shader](../../../reference/NRD-Sample/Shaders/TraceOpaque.cs.hlsl)
and [composition shader](../../../reference/NRD-Sample/Shaders/Composition.cs.hlsl)
show separate shadow sampling, frontend packing and backend visibility resolve.
The pinned [NRD frontend](../../third_party/nrd/Shaders/NRD.hlsli),
[SIGMA settings](../../third_party/nrd/Include/NRDSettings.h) and
[SIGMA integration guidance](../../third_party/nrd/README.md) define the resource
and penumbra conventions. [Ray Tracing Gems I](../../../reference/ray-tracing-gems/book.md),
chapter 13, motivates separating sampled visibility from direct shading; chapter
6 supplies the existing robust ray-origin offsets.

Focused checks compare near/far penumbrae and analytic disk visibility, preserve
lit/umbra regions, exercise cutout and tinted occlusion, and verify independent
indirect lighting. Both primary paths, raw/progressive rendering, combined
SIGMA/REBLUR/SHARC/TAA, moving blockers, sunlight changes, disabled sun and zero
bounces are relevant. Re-run stationary-edge and animated-atlas TAA regressions,
NRD dispatch coverage, ABI checks and Vulkan synchronization validation after
changes. Shader generation compiles only the used SIGMA translucency permutations.

The packed sampling data and its [MIT attribution](../../src/native/pathtrace/shaders/LICENSE-blue-noise.txt)
are checked in. `scripts/shaders/blue_noise.py` regenerates or checks the packed
header from the pinned upstream `spp8.rs` and `spp32.rs` files, passed in that
order; ordinary builds need no download.
Shadow tests record analytic error and temporal variation for complete sampling
cycles, check sampler-toggle history isolation, and inject allocation failure.
