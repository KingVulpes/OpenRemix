# EON and PBR materials

The ray-query backend supports energy-preserving Oren–Nayar (EON) diffuse
reflection combined with isotropic GGX specular reflection. Raster gameplay/UI
composition keeps its existing unlit material behavior. Geometry displacement,
refraction, anisotropy, subsurface scattering and thin film are not implemented.

The retained 0.1003 material API supplies the inputs:

| Input | Meaning |
| --- | --- |
| `MaterialInfo.albedoTexture` / `OpaqueEXT.albedoConstant` | Base color and tint |
| `OpaqueEXT.roughnessTexture` / `roughnessConstant` | Red channel, perceptual roughness in [0,1] |
| `OpaqueEXT.metallicTexture` / `metallicConstant` | Red channel, metalness in [0,1] |
| `MaterialInfo.normalTexture` | Tangent-space XYZ normal, [0,1] mapped to [-1,1] |
| `emissiveTexture`, `emissiveColorConstant`, `emissiveIntensity` | Existing visible emission and sampled area lights |

A scalar map replaces its corresponding constant. Missing maps use the constants;
missing normals use the geometric normal. The same roughness controls EON diffuse
and GGX perceptual roughness (GGX alpha is roughness squared, floored at 0.001).
Dielectrics use F0=0.04; metals use base color as F0 and suppress diffuse reflection.
EON's colored multiple scattering uses base color as its single-scattering albedo.
Isotropic BRDF evaluation uses the world-space direction/normal dot products
directly; sampling still uses the tangent frame. This preserves the EON equations
while avoiding redundant direction transforms during light evaluation.

All maps accept file paths or uploaded texture hash paths, share the material's
filter/address/sprite settings and currently sample mip zero. Roughness, metallic
and normal maps use linear data views even when their source was marked sRGB;
color/emission views of that same texture retain their original color space.
BC5 normal maps reconstruct positive Z from X/Y. The tangent frame follows the
triangle's UV derivatives, including mirrored UVs and nonuniform object transforms;
normal Y points along increasing V. Degenerate UVs use a geometric tangent frame.
Ray offsets and below-surface rejection use the geometric normal independently of
normal mapping. Raster and ray primary visibility share the material equations.

Nonzero roughness/metalness constants or any of those three data maps automatically
enable EON/GGX for opaque materials. Entirely zero-filled legacy PBR parameters
retain Lambert shading. An optional `remixapi_MaterialInfoPbrEXT` in the material
pNext chain explicitly selects or disables EON/GGX with `enabled=1/0`. Use it for
an explicitly smooth dielectric with both constants zero. This adds no fields to
the retained interface table or 0.1003 structs. Invalid constants, duplicate
extensions or invalid enable values reject a replacement without changing queued
or accepted materials.

Direct sun, analytic lights, emissive area-light sampling and every continuation
use the same material BRDF. Continuations mix the paper's CLTC-plus-uniform EON
sampler with visible-normal GGX sampling and evaluate the complete mixture PDF.
The existing emissive next-event estimator remains responsible for emissive
surface lighting; continuations do not add duplicate emitter-hit contributions.

The G-buffer adds an RGBA16F attachment (8 bytes/pixel) containing roughness,
metalness and an octahedral geometric normal. Tracing requires six color
attachments, ten storage images and 526 per-stage resources, including the
existing 512 sampled-image descriptors. Map descriptors share that existing limit.
Material/map replacements participate in scene identity and reset progressive
accumulation while preserving valid geometric motion and queued snapshots.

The existing `denoiser=reblur-sh` setting also enables REBLUR diffuse/specular
radiance filtering for opaque PBR receivers. Continuation and local-light samples
are separated into their diffuse and specular contributions using the same
sampling PDFs and visibility results; splitting the signals casts no extra rays.
NRD material factors remove base color and Fresnel response before filtering and
restore them afterward. Guides retain mapped normals, perceptual roughness,
first-bounce reflection distance, motion, and separate legacy/dielectric/metal
material IDs. Specular distance uses the nearest nonzero first-bounce distance
when multiple paths are sampled. Camera cuts and newly active histories reset
both PBR lobes.

When `localDenoiser` is enabled, PBR torch and emissive-area lighting joins these
histories; disabling it keeps the local contribution raw. Legacy surfaces retain
the existing SH and separate local-light histories. Unused legacy passes are
skipped in scenes containing only PBR receivers. Visible emission and direct sun
BRDF evaluation stay outside the radiance filter, while SIGMA continues to filter
sun visibility. TAA and FSR run after composition. Turning the denoiser off or
using progressive accumulation retains unfiltered PBR evaluation.

Four additional RGBA16F images (32 bytes/pixel, plus NRD history/pool storage)
carry specular input, diffuse/specular output, and specular material factors. PBR
diffuse input shares the existing SH0 slot. The temporal descriptor limits include
these images, and allocation failures retain the existing raw-rendering fallback.
Static PBR surfaces with sampled roughness at least 0.5 participate in the
[directional SHARC cache](sharc.md), including metals and mapped normals.
Reflection footprint checks preserve tracing for narrow lobes, and sharper hit
materials are not cached. PBR surfaces still bypass water irradiance reuse.
Transparent layers retain their existing treatment;
this filter operates on the opaque receiver behind them.

## BetaRT assets

BetaRT's `MCRTX_LOAD_PBR=0/1` launch setting controls loading all four sibling maps
(default `1`). Set it in `minecraft/mcrtx-runtime.env` and restart the client.
Disabling map loading retains base color, fallback EON parameters and procedural
block emission; it does not change the OpenRemix material API or BRDF selection.

The direct BetaRT client loads the established `_normal`, `_roughness`,
`_metallic` and `_emissive` sibling names, preferring DDS over PNG within each
source. Selected texture-pack resources precede the current texture-pack cache
and bundled `mcrtx_assets` directories. `MCRTX_ASSETS` optionally selects an asset
root. The `entities/mob` fallback and timestamp-stripped sibling names are retained.
Terrain and ordinary entity/item textures use their own siblings; compact water,
lava and portal textures use `water_*`, `lava_*` and `portal_*` siblings.

Files are copied into an immutable content-addressed runtime texture cache.
Reloading a changed map supplies a new path, leaving queued frames' textures
alive. Missing maps fall back to roughness 1, metalness 0 and interpolated vertex
normals (geometric normals when the mesh omits them).
Authored emissive maps replace procedural block masks; existing intrinsic
intensities and the torch double-lighting exclusion still apply. Ordinary
materials with an authored emissive map use unit emission intensity.

BetaRT's private material packet is version 6. Build/install the patched client
and JNI library together; the public Remix interface version remains 0.1003.

## Experimental secondary local-light reuse

`rtx.native.pathtrace.pbrLocalReuse=1` enables an experimental screen-space cache,
following NRD-Sample's use of previous-frame radiance as a first-level lookup.
The F12 **Experimental PBR lighting reuse** control selects the same option.
It is **off by default** and is not a quality-preserving replacement for tracing.
EON and GGX depend on viewing direction; even closely matched reprojection can
introduce bias and correlated noise.

Only fresh primary local-light radiance is stored. A secondary rough dielectric
hit can reuse it after position, normal, material and viewing-direction checks.
Sunlight, emission and recursive indirect tails are never taken from this cache.
Rejected hits trace fresh local lighting. Geometry/material/lighting changes,
dynamic traced instances and temporal resets prevent reuse. Sharp surfaces,
metals, blended surfaces, raw/reference accumulation and multisample rendering
use the ordinary path. Confidence retracing always uses fresh samples.

The prototype requires temporal denoising and allocates two 64-byte-per-pixel
histories only while enabled (about 253 MiB at 1920×1080). Disable/re-enable it to
retry after allocation failure. `REMIX_PBR_REUSE_DIAGNOSTICS=/path/reuse.csv`
records attempts, accepts and geometry/view rejections after each frame's fence.
Keep the option disabled for normal play until energy, temporal-noise, movement
and disocclusion comparisons meet the desired quality requirements.

## References

[EON paper](../../../reference/eon/eon.md), Listings 1–4, and its
[MIT-licensed source](https://github.com/portsmouth/EON-diffuse).
GGX sampling follows Heitz, [Sampling the GGX Distribution of Visible Normals](https://jcgt.org/published/0007/04/01/).

Previous-frame reuse is guided by [NRD-Sample TraceOpaque](../../../reference/NRD-Sample/Shaders/TraceOpaque.cs.hlsl) and its reprojection checks.
