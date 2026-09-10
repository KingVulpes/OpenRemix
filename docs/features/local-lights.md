# Analytic local lights

The ray-query backend accepts unshaped `remixapi_LightInfoSphereEXT` lights. Each
sphere is an analytic, two-sided radiance source; it is not inserted into the
geometry acceleration structures. A light contributes only in frames containing
`DrawLightInstance`. Repeated submissions of one handle do not duplicate power.
Updates validate the complete definition before replacing accepted state. The
existing public interface table and legacy structure layouts remain unchanged.

Positions and radiance must be finite, radius and RGB must be nonnegative. A zero-radius definition retains the
legacy no-op placeholder behavior; finite emitters require a positive radius. Supported numerical limits are radius <= 1e6, absolute position <=
1e12, and radiance <= 1e12. Shaping is currently unsupported and rejected. USD
light definitions retain their existing compatibility behavior.

## Selection and integration

A persistent CPU LightBVH sorts stable light IDs and builds a balanced hierarchy
using median splits along the widest axis. Membership changes rebuild topology;
position and power changes refit bounds and flux. Unchanged submissions reuse the
hierarchy and GPU buffers. Frames without active local illumination select a
cached shader specialization with the local sampler compiled out, avoiding its
register cost in ordinary sun/sky and water rendering. These variants share the
source pipeline’s layout and fence lifetime. Zero-power slots remain in the tree so redstone power
changes can use refits. GPU records use buffer device addresses and are replaced
only after the previous frame fence.

Each diffuse shading point stochastically descends the hierarchy. Node importance
uses summed sphere flux and distance relative to the node's enclosing bounds.
Only a bound wholly below the receiving plane can be culled. Nonzero children
retain nonzero probabilities; the full traversal probability weights the sample.
Selection requires logarithmic tree traversal, followed by one shadow sample,
with a default budget of two local-light samples at opaque primary surfaces and
one at continuation vertices, SHARC updates and transparent sheets. This is not
constant total frame cost: memory traffic, coherent traversal, shadow geometry,
and the number of shaded diffuse vertices still matter.

The selected sphere is sampled uniformly in solid angle. The estimator includes
both the light-selection probability and solid-angle probability. Small distant
spheres use a cancellation-resistant solid-angle calculation. Receivers inside a
virtual sphere sample its full solid angle. Shadow segments terminate at the
sampled sphere surface and retain alpha-cutout rejection and up to 32 tinted
transparent layers. There is no screen-space emitter extraction or per-light
shadow map/pass. Proxies are the only explicit emission representation, so there
is no duplicate mesh-emission contribution or competing BSDF emitter-hit PDF.

Local next-event estimation contributes at primary surfaces, ordinary diffuse
continuation vertices, and SHARC update vertices. Zero indirect bounces disables
bounced light while retaining direct local light. Transparent surfaces retain the
existing bounded sheet batches; their local radiance is resolved with the existing
transparency/TAA path.

## Denoising and temporal state

SIGMA filters the sun only. Local primary radiance has its own REBLUR_DIFFUSE
instance, packed and unpacked through NRD's radiance frontend/backend and
remodulated by albedo at composition. Indirect REBLUR_DIFFUSE_SH remains separate.
The local filter uses smaller spatial radii than GI to preserve cast shadows.
Lighting changes shorten only the local filter's history and SHARC averaging;
ordinary torch toggles do not reset global NRD/TAA history. Complete local-light
extinction invalidates old torch radiance in SHARC. Local changes also invalidate
progressive accumulation and stationary-frame reuse.

The local signal is allocated lazily on first local-light submission and retained
across unloads, avoiding repeated pipeline/history recreation at chunk boundaries.
A failed local NRD initialization retries with raw local radiance. If the separate
signal resources are unavailable, raw local and sun lighting use the established
indirect/TAA path without SIGMA, so the sun mask cannot attenuate torch radiance.
Failures are reported in optional diagnostics rather than silently presented as
active denoising.

`rtx.native.pathtrace.localLights` defaults to `1`, `localSamples` to `2`,
`localIntensity` to `1`, and `localDenoiser` to `1`. Samples accept 1–16 and
intensity accepts 0–100. F12 exposes these controls under **Torch lighting**.
`remixapi_GetLocalLightStatisticsEXT` reports light/node counts, depth, builds,
refits, buffer memory, CPU update time and the separate local NRD GPU time.
Tracing time includes local sampling and visibility; no fabricated isolated
sampling time or full-ray counters are reported.

## Minecraft adapter

BetaRT derives torch emitters from accepted terrain-section snapshots on its
existing v5 transport. Block 50 produces warm linear RGB radiance `(200,110,44)`;
block 76 produces dimmer red radiance `(40,1,0.4)`; block 75 keeps an unlit slot.
Radius is 0.08 blocks. These are artistic strengths, not physical equivalents of
vanilla's block-light levels. The center lies 0.12 blocks above the rendered cap,
including the cap's wall-orientation offsets. Rasterized torch/flame artwork is
unchanged. Other emissive blocks do not acquire lights through this feature.

Stable IDs belong to a cell within its accepted chunk/world lifetime. Stale worker
results never publish light changes. All resident torches are submitted regardless
of the raster frustum, and positions use the same floating origin as terrain.
Chunk removal retires light handles. Environment controls are
`MCRTX_PT_LOCAL_LIGHTS`, `MCRTX_PT_LOCAL_SAMPLES`, `MCRTX_PT_LOCAL_DENOISER`, and
`MCRTX_PT_LOCAL_INTENSITY`. The local-statistics export is optional for client
startup, and unrelated unavailable legacy profiling fields stay unavailable.

## References and limits

The sampling architecture follows *Ray Tracing Gems I*, chapter 18, “Importance
Sampling of Many Lights on the GPU,” including analytic emitter proxies and
stochastic hierarchy traversal. Sphere solid-angle sampling also follows the
analytic-light treatment in the local NVIDIA RTXDI sample. The pinned NRD source
and NRD-Sample provide the radiance integration contract. *Ray Tracing Gems II*,
chapter 23, “Rendering Many Lights with Grid-Based Reservoirs,” describes a possible
later improvement when spatial light selection needs more reuse.

This hierarchy has no visibility information. Nearby lights behind walls can
consume samples, causing noise in scenes with many disconnected rooms. REBLUR
and SHARC are approximate temporal filters; small features can soften, and cached
indirect light can lag rapid localized illumination changes. Reservoir reuse is
not part of this implementation.
