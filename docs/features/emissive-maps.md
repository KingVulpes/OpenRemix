# Emissive maps

The path tracer consumes `remixapi_MaterialInfo.emissiveTexture`,
`emissiveColorConstant` and `emissiveIntensity`. Emission is linear RGB. An sRGB
texture is decoded during CPU light reduction and by sRGB sampling for visible
surfaces; neither path applies the transfer twice. Its RGB is multiplied by the
constant and intensity. Without a map, the constant times intensity is used.
All coefficients must be finite and nonnegative. Rejected definitions leave the
accepted material intact. Recreate a material after replacing its texture handle,
as for albedo textures; queued draws retain their immutable texture snapshots.

Each triangle's UV region is reduced to one constant with 256 deterministic,
stratified area samples. Black texels contribute zero but remain in the average,
preserving sparse-mask coverage approximately. Texture alpha is not an additional
emission multiplier; masks should encode emission coverage in RGB. Reduction is
cached by UV region and material emission snapshot, including sprite-sheet UV
transforms. Unrelated albedo atlas animation preserves the reduction cache.
This approximation applies only to the light cast by the material. Visible glow
samples the original emission map at the surface UVs, retaining colored and black
texels. Raster primary visibility writes emission to a dedicated HDR G-buffer
attachment; ray primary visibility samples the same texture at the hit UV.
Diffuse continuation and shadow rays retain the averaged area-light model.

World triangles with nonzero emission enter the existing LightBVH as area lights.
Their power uses world-space area and luminance, with one-sided or two-sided
emission matching the instance. Transforms, skinning, material replacement and
instance removal update the sources. Sphere and triangle lights share the same
bounded shadow sample count, light selection PDFs, cutout and tinted
shadow traversal, local-light range and denoising controls. Uniform triangle-area
samples include the area-to-solid-angle Jacobian. There is no loop casting one
shadow ray for every emitter and no emission-map sampling in lighting rays.

Directly visible emission is composed separately from SIGMA and local-light
filtering. Raw, NRD/TAA, FSR and progressive modes share the same source values.
At diffuse continuation vertices, next-event estimation accounts for emitter
illumination; intrinsic emission is not added again on a secondary emitter hit.
SHARC stores reflected outgoing lighting under this same convention. Zero indirect
bounces still permits directly sampled emissive lighting. The Local lights toggle
disables casting while retaining visible emission. Under F12 **Local lighting**,
**Torch lights** controls analytic sphere casting and **Emissive lighting**
controls triangle casting independently. Both default on; their configuration
keys are `rtx.native.pathtrace.torchLights` and `emissiveLights`. Neither hides
visible emission. Disabled sources leave the sampling hierarchy, so they cannot
consume proposals intended for the remaining lights.

For more than four light slots including emissive triangles, bounded stratified
resampled importance sampling compares four unshadowed candidates per requested
opaque primary local-light sample. Indirect vertices, SHARC updates, confidence
retracing and transparent sheets retain the established sampler, avoiding repeated
candidate work throughout the continuation/transparency stack.
Each candidate descends the LightBVH and samples the selected
emitter's area or solid angle. Its RGB contribution is divided by the full BVH
proposal probability; a reservoir chooses among candidates by luminance. For candidate
estimates `e_i`, weights `w_i = luminance(e_i)` and candidate count `M`, the
returned estimate is `e_selected * sum(w_i)/(M*w_selected) * visibility`.
Zero candidates remain in `M`. Stratifying the BVH selection variate retains the
expected light integral while reducing repeated proposals from one cluster.
There is still at most one fresh shadow segment per local-light sample. Candidate
work is bounded, with `M` logarithmic BVH traversals rather than an emitter loop.

`rtx.native.pathtrace.emissiveCandidates` accepts 1–8, default 4. Setting 1 restores
ordinary BVH sampling with optional visibility guidance. The existing two-to-four
light reservoir and sphere-only shaders are preserved. With visibility guidance
enabled, a cell whose preferred visible source is an emissive triangle can propose
from its ancestor subtree of at most 32 lights. This spreads proposals over nearby
triangles of a tessellated emitter. Torch-dominated cells keep whole-tree RIS.
The proposal mixes conditional-subtree and whole-tree sampling, evaluated at
the current receiver. Its guide weight `g` is at most 0.75, retaining at least 25%
whole-tree exploration. A leaf inside the subtree has probability
`p_BVH(leaf) * (1-g + g/p_BVH(subtree))`; other leaves retain
`(1-g)*p_BVH(leaf)`. The full mixture probability is used before reservoir selection.
Every winner receives a fresh visibility ray. Sparse winner feedback identifies
visible sources and whether guiding is useful, but does not estimate the base
proposal's blocked fraction. The guide ramps in as observed winner occlusion rises
from 2% to 20%, using up to 256 retained observations. Fully visible scenes keep
whole-tree stratification, avoiding unnecessary concentration on one cluster.
Switching between RIS and ordinary guiding invalidates their differing feedback.
Lazy shader specialization removes its register cost from other scenes, with
ordinary BVH sampling as the pipeline-creation fallback.

Resampling reduces unoccluded contribution variance, including competition
between torches and triangle emitters. Guidance can favor a previously visible
emissive group, but neither technique eliminates noise from unknown
occlusion, differing light colors or near-field triangle-area sampling, and adds
candidate arithmetic even though the visibility-ray budget is unchanged.

F12 **Torch / emissive lighting** optionally separates opaque primary direct
lighting into two LightBVHs and two independent REBLUR diffuse histories.
`rtx.native.pathtrace.separateLocalLighting` selects **Combined** (`0`, default),
**Separate - shared ray budget** (`1`), or **Separate - samples per family** (`2`).
Combined retains the established sampler. Separation is
experimental: independent histories can isolate fluctuations between source
families, but do not guarantee less noise and require extra filtering and memory.
Indirect lighting, SHARC, confidence retracing and transparent sheets retain the
combined hierarchy. SIGMA continues to process only the sun.

Shared-budget mode assigns complementary pixels to torch or emissive sampling
with a changing frame-wide phase. Where both families can contribute, each has
probability one half and its radiance is multiplied by two; the other signal is
zero. NRD distance reconstruction and a small prepass handle those missing samples.
Per-family mode allows the configured sample count for each contributing family,
up to twice the primary shadow-ray budget, without that selection step. Both modes
retain fresh visibility, full selection PDFs, and the same physical light powers.
A blocked sample retains its hit distance; only a missing sample has no distance.
The emissive tree has its own visibility proposal history. Torch sampling uses
fresh candidates without sharing the emissive guide.

With temporal resources active, split direct sampling runs in a separate compute
pass after opaque shading. Existing scratch images retain full-precision hit
positions and half-precision normals until that pass finishes, avoiding another
primary ray or depth-based position reconstruction. Per-family lighting generations drive their
fast-response windows. Two additional RGBA16F signal images and a second local
NRD instance are allocated only when separation is requested and supported.
Capability or initialization failures fall back to combined local lighting.
The **Denoise local lighting** control applies to both direct histories; raw
rendering can still use either split sampler without the filtering resources.

The existing BetaRT transport already supplies masks for intrinsically luminous
blocks. Its normal and redstone torches retain their analytic sphere sources;
their material emission is disabled to avoid duplicate lighting. This feature does
not add a texture-pack filename convention or an automatic emissive suffix loader.

Private frame and emitter records are extended; public API layouts and exports
are unchanged. The existing G-buffer previous-position image is reused as emission
composition scratch only after opaque motion reconstruction. The visible-emission
attachment adds storage but no additional render pass. Capability checks account
for the fifth color attachment and ninth tracing storage-image descriptor.
Emission colors and source changes participate in scene and accumulation change
detection without imposing a global temporal reset on ordinary lighting updates.

Transport updates have a separate generation from temporal lighting response.
Every animated-map color change still updates the sampled radiance and invalidates
progressive accumulation. The scene-wide fast-response window is reserved for
source membership/geometry changes, on/off transitions, or a combined RGB-power
change above 25% relative to the last significant snapshot. Power uses the same
sphere-radius/triangle-area scaling as the LightBVH. Comparing to an anchor catches
gradual ramps; summing changes across the scene prevents one animated portal from
continually shortening the history of unrelated torches. Local radiance changes
retain normal NRD history and anti-lag behavior. SHARC uses the significant-lighting
generation, while visibility guidance retains its spatial correspondence through
radiance-only updates. These generations do not change public or GPU record layouts.
