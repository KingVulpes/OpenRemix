# Optional diffuse path tracing

Raster remains the startup default. The path tracer replaces world geometry shading
while reusing the native runtime's window, swapchain, input, resources and raster
gameplay/UI composition. It rasterizes primary visibility into a G-buffer and uses a compute shader with
`VK_KHR_ray_query` for shadow and indirect rays. It does not use the removed RTX renderer. Normal builds use the checked-in SPIR-V and need no shader
compiler or external denoiser installation. NRD 4.18.0 is statically linked by default.

## Selection and API

After Startup, select `rtx.native.backend=raster|pathtrace` through
`SetConfigVariable`, or use the native debug panel. The sample accepts
`LIBRARY FRAME_COUNT pathtrace`. Selection is applied only after the preceding
frame fence completes. Tracing allocations are created on selection and released
on switching back. A resource initialization failure records a reason and retains
raster. Submission/device failures retain the existing terminal-failure contract.

`remixapi_GetBackendCapabilitiesEXT` reports supported, requested and active
backends plus a missing-capability reason. Initialize `structSize` to `sizeof` the
extension struct. Unsupported selection returns `FEATURE_UNAVAILABLE`; invalid
values return `INVALID_ARGUMENTS`. The NRD bit reports compiled and independent device capability; upscaling and DLSS bits remain clear.
The original C/C++ 0.1003 headers, interface layouts and calling conventions remain
unchanged; the new exports and instance extension live in `remix_native_ext.h`.

Required capabilities are Vulkan 1.2, `VK_KHR_acceleration_structure`,
`VK_KHR_ray_query`, `VK_KHR_deferred_host_operations`, acceleration structures,
buffer device addresses, shader int64, nonuniform sampled-image array indexing,
512 sampled-image descriptors, 524 compute-stage resources, two AS descriptors,
eight storage-image descriptors, four color attachments, storage/color-attachment support
for RGBA32F, RGBA16F, RGBA8 and R32F, RGBA32UI storage support, and a
graphics/compute/presentation queue. Supported tracing features are enabled when the Vulkan device is created. Raster still requires
only its previous Vulkan 1.1 graphics/presentation capabilities.

| Config suffix under `rtx.native.pathtrace.` | Range | Default |
| --- | --- | --- |
| `primaryVisibility` | `raster`, `ray` | raster |
| `samples` | 1–16 samples per pixel per presentation | 1 |
| `bounces` | 0–4 indirect diffuse bounces | 2 |
| `accumulate` | `0`, `1`, `false`, `true` | false |
| `denoiser` | `off`, `reblur-sh` | reblur-sh |
| `taa` | `0`, `1`, `false`, `true` | true |
| `validation` | Same booleans; NRD validation overlay | false |
| `forceRebuild` | Same booleans; rebuild dynamic TLAS for profiling | false |
| `sky` | Same booleans; indirect environment illumination | true |
| `sharc` | Same booleans; [diffuse radiance cache](features/sharc.md) | true |

The last distant light drawn during a frame is selected, including zero radiance.
Direction specifies the direction light travels; radiance and color are linear.
The existing angular diameter sets sun size; BetaRT uses 0.533 degrees. Primary
opaque sun visibility is filtered separately by [SIGMA](features/sigma.md), while
REBLUR diffuse SH retains indirect lighting. Zero diameter retains hard shadows.
No submitted distant light means no direct sunlight. The sample draws white daylight;
BetaRT updates its white distant light from the visible sun each world frame. The debug panel can override its normalized
direction, color and intensity. Other light types and automatic persistent-light
submission retain their compatibility no-op behavior.

## Shared ownership and scene identity

`NativeResources` validates and copies mesh, material, texture, camera and draw
submissions for both backends. Queued draws hold the exact resource versions they
resolved. Destroying or replacing a registration cannot change an already queued
draw. Byte-identical texture/material/mesh submissions retain the existing version.
Texture and shading fingerprints make accumulation comparisons independent of
full texture/mesh sizes during steady frames.

Optional `remixapi_InstanceIdentityEXT` specifies a nonzero stable ID, a static,
dynamic or raster-only classification, and independent raster visibility. IDs
must be unique across traced mesh submissions in a frame; GPU-instancing elements
add their array index. Surfaces are geometries within that mesh's BLAS. Callers
without the extension get collision-free frame-local dynamic IDs; their draw order
is not a persistent object identity. Zero IDs are allowed only for raster-only
instances when explicitly using the extension. Sky and viewmodel categories remain
raster-only. Hidden/ignored/first-person-shadow categories retain their prior skip
behavior. Client submissions must explicitly classify gameplay feedback.

BetaRT private JNI protocol version 5 carries lifetime IDs, classification, raster
visibility, object transforms, separate camera/floating-origin data, and camera world/reset epochs. Every camera packet carries the epochs, so replacing a pending frame cannot lose a reset. JNI still
copies packets before returning and retains ordered resource updates plus the
bounded frame mailbox. Terrain IDs use world generation, section lifetime and mesh
pass. Entity IDs use entity lifetime, model part and render pass; replacing a model
part mesh preserves its part identity. Entity keys use weak object identity so
reused vanilla network IDs cannot alias a previous lifetime. Labels, damage/selection overlays and fire
effects use raster-only scopes.

The client anchors object transforms to a 256-block snapped floating origin,
rebasing after camera displacement exceeds 1,024 blocks on an axis. Camera motion
within that region changes camera data only. Rebase changes object transforms,
never mesh vertices. All terrain in loaded-section residency and entities within
that residency radius are submitted for tracing, independently of the camera
frustum. Raster visibility remains a separate predicate.

## Acceleration structures

Rigid geometry is keyed by triangle positions, indices and surface topology,
independently of UVs, textures, tints and material bindings. Static meshes and shared
rigid entity parts build with `PREFER_FAST_TRACE | ALLOW_COMPACTION`. A replacement
mesh can acquire the old geometry before its retired view is evicted. Compaction
queries are read after fence completion, compact copies are recorded on the next
frame, and replaced allocations survive until that copy's fence completes.

Skinned vertex deformation owns a separate `ALLOW_UPDATE` BLAS per instance.
Unchanged bones skip work. Changed bones refit the immutable topology; geometry
replacement rebuilds. A bounds revision forces the corresponding dynamic TLAS
update even when the BLAS address and overall bounds diagonal remain unchanged.
Absent deformation instances retire after their last-use fence.

Every ray queries both a static TLAS and a dynamic TLAS, choosing the nearest accepted
intersection. Static membership, geometry-address and transform changes rebuild the
static TLAS. The dynamic TLAS uses `ALLOW_UPDATE | PREFER_FAST_TRACE`: unchanged
inputs skip work, ordinary motion refits, membership/address changes rebuild.
Capacity is distinct from actual instance count. Singular transforms are excluded;
crossing between singular and active transforms changes membership and rebuilds.

Dynamic recovery rebuilds occur after 300 refits or displacement from the last build
greater than `max(16 blocks, 4 × transformed bounds diagonal)`. These are initial
heuristics, not a universal performance claim. Forced-rebuild mode affects only the
dynamic TLAS. Rebuild reasons and refit counts are observable.

AS storage suballocates 16 MiB arenas; larger requests get larger arenas. Independent
BLAS builds share a batch with nonoverlapping aligned scratch ranges. TLAS scratch
is separate. Barriers order upload writes → BLAS/compaction writes → TLAS builds →
ray queries → composition. The runtime retains one frame in flight and performs no
per-mesh queue-idle wait. BetaRT keeps existing section publication budgets (four
sections, 8 MiB or 1 ms per iteration) and publishes replacement geometry before its
trace dispatch; the previous version remains usable until that publication.

## Integrator and composition

The default primary pass rasterizes the traced scene's triangles into four attachments:
world position/depth (RGBA32F), geometric normal/surface flags (RGBA16F), linear
albedo/opacity (RGBA8) and previous world position/history validity (RGBA32F).
It uses the tracer's existing geometry, skinned vertices and material table, with
shared texture/alpha evaluation and perspective-correct interpolation. Material
color is quantized to eight bits in this pass. All traced submissions participate;
`rasterVisible` remains independent of tracing, and offscreen geometry stays in the
acceleration structures for shadows and indirect rays. Raster clip planes apply to
primary visibility.

The jittered G-buffer supplies the first hit to the lighting compute shader.
Transparent first hits retain ray traversal through subsequent sheets and the
opaque background. This preserves the existing bounded transparency model. A
second, unjittered raster pass writes stable opaque/cutout depth for gameplay
composition, excluding blended sheets. No primary or depth ray is needed for an
opaque pixel. Sparse previous-frame confidence queries remain ray traced.

`primaryVisibility=ray` keeps the original camera/depth-ray implementation for
manual comparison. The debug panel exposes the same switch; changing it resets
both temporal history and progressive accumulation. GPU tracing timestamps include
the G-buffer and depth passes in raster-primary mode. The five extra attachments
use 48 bytes per pixel, excluding depth, allocation padding and driver overhead.
The existing depth attachment is reused between passes.

The compute shader reconstructs secondary-hit triangle positions, UVs, vertex tints
and geometric normals. Textures respect their linear/sRGB format, nearest/linear filtering and
address modes; compute shading currently samples mip level zero. At each diffuse
surface it samples the distant light with a shadow ray and continues along a
cosine-weighted hemisphere. Barycentric reconstruction and geometric-normal ULP
offsets follow *Ray Tracing Gems*, chapter 6. Mirrored transforms preserve the raster
front-face convention. Double-sided surfaces orient the normal toward the incident ray.

Cutouts remain opaque denoising inputs. A frame-wide Halton camera jitter selects
one shared primary surface per pixel through raster coverage; all configured samples launch independent
diffuse continuations there. Primary direct sunlight is kept separate. Indirect
radiance is demodulated by the primary material, packed with upstream SH helpers
using the first continuation direction and normalized hit distance, filtered by
REBLUR_DIFFUSE_SH, and resolved with the sample's high-quality SG resolve and diffuse
re-jittering. Both SH outputs are used before restoring material factors. Full-rate
sampling disables hit-distance reconstruction; roughness is linear and fixed at one.

Blended primary sheets are excluded from opaque guides. Opaque lighting and transparent
composition use separate compute dispatches. The first walks through at most 32
sheets and shades the opaque background with the initial RNG sequence used by
confidence retracing. The second walks the sheets again in batches of at most eight
position/normal/color records, shades them in the original order, and combines
straight-through tint and opacity with the denoised opaque background. The smaller
records are never live across opaque shading or NRD guide generation.

The existing transmission image uses RGBA32UI storage between dispatches for exact
background-radiance and RNG bits. World-color alpha temporarily carries the signed
sheet count (negative means the layer bound blocked the background), preserving
previous RGB for progressive accumulation. A compute write-to-read/write barrier
orders the dispatches. Composition restores stable depth and clears transmission
before the retained raster multiply/add passes. No extra per-pixel allocation,
lighting sample, public interface field or history-reset condition is introduced.

Distant-light shadow rays retain deterministic opacity/tint products; indirect continuations use stochastic transmission. The translucent
material fallback still multiplies alpha by 0.2. The layer bound absorbs remaining
rays. There is no ray bending or specular material model.

Optional `remixapi_CameraEnvironmentEXT` on the world camera supplies an enabled
flag and finite, nonnegative linear RGB sky radiance. Invalid metadata rejects the
whole camera submission before changing matrices, epochs or environment state.
Absent or disabled metadata selects black; sky/viewmodel submissions leave world
environment state intact. The CPU/GPU frame appends one aligned radiance vector.
Each existing diffuse continuation that escapes adds throughput times this uniform
radiance, including cutout rejection and stochastic tinted transmission. No extra
sample, pass, screen lookup or cubemap is used. The existing bounce/layer bounds
and NRD SH packing still apply; zero indirect bounces disables sky illumination.

BetaRT reads the same interpolated sky RGB and celestial angle as its rasterized
sky. It applies the sRGB transfer to sky RGB exactly once, at unit strength. With
`theta = 2*pi*celestialAngle`, sun travel direction is `(0, -cos(theta), -sin(theta))`;
white radiance is `pi * clamp(2*cos(theta), 0, 1) * (1-rain) * (1-thunder)`.
Sunlight fades continuously to zero at both horizons. Dimensions without a sky
submit disabled sky and zero sunlight. Night ambient comes solely from Minecraft's
night-sky color. The sky toggle is independent of the manual daylight override.

Sky renders to linear HDR. Resolved opaque light, transparent light and native TAA
compose there before raster particles, weather, labels, held items, selection and
UI. Native-resolution TAA reconstructs the jittered samples at fixed display pixel
centers and stores color history on that unjittered grid. Color reprojection uses
nonjittered motion; depth/normal validation separately reprojects the selected
guide with the current-minus-previous jitter offset and checks matching geometry
within the bilinear footprint. It uses neighborhood variance
clipping and responsive transparent history. This is TAA, not temporal upscaling
(TAAU). Camera jitter is enabled only with an active TAA resolve or progressive
accumulation; raw tracing, NRD-only rendering and temporal initialization failure
use stable pixel centers. The existing display transfer runs once on world
output; there is no new tone mapper. Stable unjittered raster depth respects
cutouts and skips blended sheets for raster occlusion. The optional ray-primary
reference mode obtains that depth from center rays. Coplanar feedback uses biased
LEQUAL; held items and UI retain their depth layers.

## Temporal ownership and confidence

History belongs to the last successfully submitted GPU frame, including retained
previous deformation buffers through its fence. Persistent IDs provide transforms
and barycentric vertex correspondence. New/frame-local instances and mesh replacements
invalidate geometric correspondence. Material/alpha edits, texture uploads and sprite
animation retain geometric motion: updating an animated tile in a shared terrain atlas
must not discard history for every block using that atlas. Depth/normal rejection,
lighting confidence and TAA color clipping handle changes in the visible pixels;
transparent composition also uses responsive history blending.
Reordered or identical draws, camera motion and ordinary animation preserve global
history. Optional `remixapi_CameraTemporalEXT` supplies `worldEpoch` and
`historyResetEpoch`; change the latter for explicit cuts and floating-origin rebases.
Resolution and integrator/mode changes also reset history.

Motion is previous-minus-current screen position in pixels plus view-depth delta.
NRD receives nonjittered matrices and separate current/previous pixel jitter. Sparse
lighting confidence retraces previous samples with their previous seeds in the
current scene at one-fifth width/height, then applies five geometry-aware blur passes.
This detects illumination changes from moving offscreen occluders and updated sky
radiance. SHARC uses matching cache queries during confidence retracing. Ordinary time/weather and sky-toggle changes update scene
identity and progressive accumulation, while preserving global temporal history.
REBLUR retains pinned defaults apart from full-rate reconstruction, enabled confidence
and history length derived from actual frame time and the SDK accumulation duration.

Progressive accumulation is the raw reference mode. While enabled it bypasses NRD
and TAA and preserves their requested settings. It resets on camera, scene, light,
texture and integrator changes; temporal denoising uses its separate validity model.
Temporal redraws warm unchanged histories, then cache world output for UI-only
redraws. BetaRT reports temporal redraws independently from fresh game frames.

## Diagnostics, limits and verification

`remixapi_GetDenoisingCapabilitiesEXT` independently reports TAA/REBLUR support and missing requirements. `remixapi_GetDenoisingStatisticsEXT` reports requested/active modes, failure reasons, reset/history/rendered counts, convergence redraws, allocated/peak temporal memory and previous completed confidence/NRD/TAA GPU timings. The TAA interval also includes final resolve and guide-history copies, so REBLUR-only reports its copy cost there. Initialize each `structSize`. Recoverable temporal initialization failures leave raw tracing active; device loss follows runtime recovery.

Temporal capabilities additionally require extended storage formats, 33 storage-image descriptors, 550 per-stage resources, sampled/storage/filtering formats, and NRD storage-image reads/writes without format declarations. These checks are independent of ray-query support. Resources are allocated lazily after the frame fence.

The panel and `remixapi_GetTracingStatisticsEXT` expose samples, accumulation,
scene/sample generations, cumulative BLAS builds/refits/compactions, static TLAS
builds, dynamic TLAS builds/refits/reasons, acceleration bytes and GPU timestamps.
Counters are scoped to the current backend activation. Timings are from the previous
completed frame and expose an availability flag.
Acceleration bytes include AS arenas, scratch and TLAS instance buffers, not total
texture, shading, framebuffer or driver memory. BetaRT can export per-presentation
CSV with `MCRTX_PT_PROFILE`; see its profiling script.

The tracer supports up to 512 distinct material texture/sampler descriptors in a
frame, within the existing resource/draw input limits and the device's AS limits.
Exceeding a runtime allocation/resource limit fails Present using the existing
failure contract. Acceleration structures are not serialized to disk.

`tests/pathtrace/rendering.cpp` covers capability fallback, caching, camera-only
motion, rigid/deformed animation, recovery policy, queued mesh replacement,
UV-only changes, identity collisions, accumulation, light changes, offscreen
shadows, alpha cutouts, tinted transmission, diffuse color transfer, large
coordinates, raster depth, switching and resize. The dedicated TAA stability test
measures stationary slanted-edge travel and intermediate coverage over every
Halton phase, with raster/ray primary visibility and raw, NRD-only, TAA-only and
combined modes. Its shared-atlas variant updates an unseen texture tile every frame
while retaining the same visible edge. `REMIX_TEST_PRIMARY_VISIBILITY=ray` runs the general scene/quality
tests with the reference primary path. The lifecycle and submission-fault tests
also run with tracing selected, and compute-pipeline initialization failure
checks raster fallback. Existing ABI/lifecycle and fault tests remain enabled.
Keep hardware results, paired client benchmarks and TAA edge measurements in
local reports under `_work/reports/`.

Build with `-Dnrd=false` to omit the static NRD CPU library and SH dispatches; native TAA remains available. All shader binaries are checked in. See [shader regeneration](shader-toolchain.md) for pinned DXC and validation commands.

SHARC integration, controls, SDK provenance and additional book/sample references are
described in [the cache feature guide](features/sharc.md).

## References

Primary reference: [local Ray Tracing Gems](../../reference/ray-tracing-gems/book.md),
chapters 3 and 19 (AS and retained scenes), 6 (robust origins), and 15–16 (sampling).
The implementation was also informed by ZetaRay's static caching/compaction
(`Source/ZetaCore/RayTracing/RtAccelerationStructure.cpp`), MCVR's chunk submission
(`src/core/render/chunks.cpp`), and Radiance's world-render integration
(`WorldRendererMixins.java`) in the sibling reference checkouts.

Vulkan contracts: [ray queries](https://docs.vulkan.org/refpages/latest/refpages/source/VK_KHR_ray_query.html),
[AS build/update rules](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html),
and [instance facing](https://docs.vulkan.org/refpages/latest/refpages/source/VkGeometryInstanceFlagBitsKHR.html).
The profiling comparison measures total frame cost as recommended by
[NVIDIA's RTX practices](https://developer.nvidia.com/blog/best-practices-for-using-nvidia-rtx-ray-tracing-updated/).
