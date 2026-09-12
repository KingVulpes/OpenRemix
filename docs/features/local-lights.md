# Analytic local lights

[Emissive maps](emissive-maps.md) also contribute triangle area sources to this
hierarchy. The sphere-specific transport described here remains the torch path.

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
position and power changes refit bounds and flux. Unlit slots do not expand the
active influence bounds. Unchanged submissions reuse the
hierarchy and GPU buffers. Frames without active local illumination select a
cached shader specialization with the local sampler compiled out, avoiding its
register cost in ordinary sun/sky and water rendering. These variants share the
source pipeline’s layout and fence lifetime. Zero-power slots remain in the tree so redstone power
changes can use refits. GPU records use buffer device addresses and are replaced
only after the previous frame fence.

Each diffuse shading point stochastically descends the hierarchy. Sets of two
to four submitted light slots use the [candidate reservoir](torch-stability.md)
while retaining one visibility query. Node importance
uses summed sphere flux and distance relative to the node's enclosing bounds.
The squared distance used for importance is at least one percent of the squared
bounding-sphere radius. Nearest-box distance alone becomes zero inside a cluster
and can give its entire flux an emitter-sized denominator, starving sibling
clusters and producing rare, enormous inverse-PDF samples. This distance floor
regularizes only the selection estimate. Exact range attenuation, sphere radiance
and the full selection PDF still determine the returned light energy. It adds no
visibility rays or candidate traversals and applies consistently to primary,
indirect, transparent, SHARC and confidence sampling.
Only a bound wholly below the receiving plane can be culled. Nonzero children
retain nonzero probabilities; the full traversal probability weights the sample.
Selection requires logarithmic tree traversal, followed by one shadow sample,
with a default budget of one local-light sample at opaque primary surfaces,
continuation vertices, SHARC updates and transparent sheets. This is not
constant total frame cost: memory traffic, coherent traversal, shadow geometry,
and the number of shaded diffuse vertices still matter.

The selected sphere is sampled uniformly in solid angle. The estimator includes
both the light-selection probability and solid-angle probability. Small distant
spheres use a cancellation-resistant solid-angle calculation. Receivers inside a
virtual sphere sample its full solid angle. Shadow segments terminate at the
sampled sphere surface and retain alpha-cutout rejection and up to 32 tinted
transparent layers. Local visibility queries stop at any accepted opaque blocker
and reconstruct only the nearest transmitting sheet across the static and dynamic
scene structures. Unconditional opaque blockers need no texture lookup, and shadow
queries do not compute surface motion/history data. There is no screen-space emitter extraction or per-light
shadow map/pass. Emissive triangle sources use area sampling in the same hierarchy;
secondary emitter hits do not duplicate their next-event contribution.

Local next-event estimation contributes at primary surfaces, ordinary diffuse
continuation vertices, and SHARC update vertices. Zero indirect bounces disables
bounced light while retaining direct local light. Transparent surfaces retain the
existing bounded sheet batches; their local radiance is resolved with the existing
transparency/TAA path.

## Influence bounds and visibility guidance

`localRange` defaults to 16 world units (blocks in BetaRT). For distance squared
`d2` from the receiver to the emitter center and reach `R`, the additional
attenuation is `(1-t)^2 * (1+2*t)`, where
`t = clamp((d2/R^2 - 9/16) * 16/7, 0, 1)`. Individual emitter radiance is unchanged
inside `0.75*R`, fades with zero endpoint slope, and is black at and beyond `R`.
Zero reach restores the unlimited sphere model. Positive reach accepts 0.01–1024.
This is an explicit artistic attenuation model: scenes can be darker after
removing the collective contribution of distant lights. LightBVH bounds use a
conservative nearest-distance test, and the sampled emitter receives the exact
attenuation. All direct, transparent, continuation and SHARC paths agree.

`localGuiding` defaults to enabled. Opaque primary local-light sampling consults
previous-frame visibility from 2-unit spatial cells with six normal-direction
bins. Each cell can prefer a recently visible emitter. Feedback from the base
LightBVH distribution estimates the blocked fraction and controls a mixture
weight up to 0.75; at least 25% of the proposal remains exploration. Cells with
insufficient evidence or little occlusion use ordinary LightBVH sampling.
Every chosen light receives a fresh finite shadow ray. The estimator divides by
the full mixture probability, `(1-g)*p_BVH(light) + g*[light == preferred]`, and
its sphere solid-angle PDF. Previous visibility never directly supplies light or
shadow intensity. Cached-leaf probabilities are evaluated at the current receiver.
This is proposal guidance, not a reused radiance reservoir or visibility cache.
The [emissive RIS sampler](emissive-maps.md) uses a separate interpretation of
the same feedback storage: visible area-source subtrees and a bounded mixture,
without treating resampled winners as base blocked-fraction observations.

Feedback is sampled at 1/64 of opaque primary light samples. Two 65536-slot buffers
hold immutable previous-frame data and current atomic feedback, about 2 MiB total.
The first trainer in an observed cell carries forward at most 32 base observations,
rescaling their visibility sum to the retained count. Its previous preferred-light
score decays by one quantization bin before competing with fresh visible samples.
This avoids an oscillation in sparse cells: enabling guidance reduces base samples,
which otherwise makes the cell lose its minimum evidence on the following frame.
Initialization and training both use atomics, so simultaneous trainers cannot lose
updates. Unobserved cells expire; no additional pass or history buffer is needed.
Hash collisions or missing cells lose only guidance. Frame submission swaps the
banks; fence completion owns readback and reuse. A light membership, position or
shape change, range change or world/camera history discontinuity invalidates
guidance. Radiance-only updates retain visibility evidence and still evaluate the
current proposal PDF and a fresh shadow ray. Geometry
visibility changes retain exploration and do not require global history resets.
Only the base-distribution feedback estimates visibility, preventing guided
successes from distorting that estimate. A single active light does not initialize
or update guidance; existing buffers are retained for reuse. Unsupported node
counts (2^20 nodes or more) use ordinary sampling. The guided shader variant is
compiled on demand; scenes without guidance use the smaller ordinary shader.
History allocation or guided-shader initialization failure falls back to ordinary
sampling and reports the reason.
An optional in-memory Vulkan pipeline cache retains compiled data across target
and filter recreation for the lifetime of the tracing device.

Guidance affects opaque direct light only. Indirect paths, SHARC updates,
confidence retracing and transparent sheets retain their existing independent
sampling, preserving their temporal contract. The guide improves sample utility
in occluded scenes but has traversal and memory overhead; it is not guaranteed to
reduce GPU time at an unchanged sample count. One primary sample is the new default;
raise it for cleaner raw output or particularly difficult soft shadows.

## Denoising and temporal state

SIGMA filters the sun only. Local primary radiance has its own REBLUR_DIFFUSE
instance, packed and unpacked through NRD's radiance frontend/backend and
remodulated by albedo at composition. Indirect REBLUR_DIFFUSE_SH remains separate.
The local filter uses smaller spatial radii than GI to preserve cast shadows.
[Torch stability](torch-stability.md) describes the candidate estimator and
existing NRD outlier-suppression settings.
Significant lighting changes shorten only the local filter's history and SHARC averaging;
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

`rtx.native.pathtrace.localLights` defaults to `1`, `localSamples` to `1`,
`localIntensity` to `1`, `localDenoiser` to `1`, `localRange` to `16`, and
`localGuiding` to `1`. Samples accept 1–16 and
intensity accepts 0–100. F12 exposes these controls under **Local lighting**.
`torchLights` and `emissiveLights` independently control sphere and triangle
casting beneath the existing master switch, both defaulting to `1`. Larger sets
containing emissive triangles use the bounded [emissive candidate reservoir](emissive-maps.md);
`emissiveCandidates=1` restores ordinary BVH sampling and visibility guidance.
`remixapi_GetLocalLightStatisticsEXT` reports light/node counts, depth, builds,
refits, buffer memory, CPU update time and the separate local NRD GPU time.
Tracing time includes local sampling and visibility; no fabricated isolated
sampling time or full-ray counters are reported. It also includes existing sun,
indirect and transparent-surface tracing; compare enabled and disabled lighting
to measure an incremental cost. The local NRD filter has its own timer.
`remixapi_GetLocalLightGuidingStatisticsEXT` is a separate optional export. It
reports guide activity, capacity, memory, resets and sampled queried/guided/visible
counts. Those counters observe 1/64 of samples and are not full ray counts; the
existing statistics structures retain their layouts.

## Minecraft adapter

BetaRT derives torch emitters from accepted terrain-section snapshots on its
existing v5 transport. Block 50 produces warm linear RGB radiance `(200,110,44)`;
block 76 produces dimmer red radiance `(40,1,0.4)`; block 75 keeps an unlit slot.
Radius is 0.08 blocks. These are artistic strengths, not physical equivalents of
vanilla's block-light levels. The center lies 0.12 blocks above the rendered cap,
including the cap's wall-orientation offsets. Rasterized torch/flame artwork is
unchanged. Other intrinsically luminous blocks use the separate emissive-map
material path. Torch material emission is disabled to keep a single source of
torch lighting.

Stable IDs belong to a cell within its accepted chunk/world lifetime. Stale worker
results never publish light changes. All resident torches are submitted regardless
of the raster frustum, and positions use the same floating origin as terrain.
Chunk removal retires light handles. Environment controls are
`MCRTX_PT_LOCAL_LIGHTS`, `MCRTX_PT_LOCAL_SAMPLES`, `MCRTX_PT_LOCAL_DENOISER`,
`MCRTX_PT_LOCAL_INTENSITY`, `MCRTX_PT_LOCAL_RANGE`, and
`MCRTX_PT_LOCAL_GUIDING`. The local-statistics export is optional for client
startup, and unrelated unavailable legacy profiling fields stay unavailable.

## References and limits

The sampling architecture follows *Ray Tracing Gems I*, chapter 18, “Importance
Sampling of Many Lights on the GPU,” including analytic emitter proxies and
stochastic hierarchy traversal. Sphere solid-angle sampling also follows the
analytic-light treatment in the local NVIDIA RTXDI sample. The pinned NRD source
and NRD-Sample provide the radiance integration contract. *Ray Tracing Gems II*,
chapter 23, “Rendering Many Lights with Grid-Based Reservoirs,” describes a possible
later improvement when spatial light selection needs more reuse.

[Epic's MegaLights overview](https://dev.epicgames.com/documentation/en-us/unreal-engine/megalights-in-unreal-engine)
informs the use of bounded influence, recent-visibility guidance and continuing
exploration. The implementation here is a small LightBVH proposal mixture, not the
MegaLights renderer. Camera distance alone is not used to discard contributing
lights.

The base hierarchy has no visibility information, and guidance can be absent,
stale, or shared across a cell containing multiple surfaces. Lights behind walls
still consume exploration samples. At an unchanged budget, guidance may add cost
in mostly unoccluded scenes. REBLUR
and SHARC are approximate temporal filters; small features can soften, and cached
indirect light can lag rapid localized illumination changes. Reservoir reuse is
not part of this implementation.
