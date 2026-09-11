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
bounded sample count, light selection PDFs, visibility guiding, cutout and tinted
shadow traversal, local-light range and denoising controls. Uniform triangle-area
samples include the area-to-solid-angle Jacobian. There is no loop casting one
shadow ray for every emitter and no emission-map sampling in lighting rays.

Directly visible emission is composed separately from SIGMA and local-light
filtering. Raw, NRD/TAA, FSR and progressive modes share the same source values.
At diffuse continuation vertices, next-event estimation accounts for emitter
illumination; intrinsic emission is not added again on a secondary emitter hit.
SHARC stores reflected outgoing lighting under this same convention. Zero indirect
bounces still permits directly sampled emissive lighting. The Local lights toggle
disables casting while retaining visible emission.

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
