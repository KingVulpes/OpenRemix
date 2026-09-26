# OpenUSD captures, mods, and the Linux editor viewport

OpenRemix can load static Remix captures and authoring projects independently of
Godot. Build with `-Dusd=true -Dusd_root=/path/to/OpenUSD-25.11`; supply
`-Djson_include=/path/to/nlohmann/include` and, when needed,
`-Dusd_tbb_root=/path/to/matching/TBB`. `usd_gnu_stl=true` matches the legacy
TfHashMap ABI of a GCC-built OpenUSD when the runtime is compiled with Clang.
The default `usd=false` build has no USD dependency. Its USD extension exports
return `FEATURE_UNAVAILABLE`. The CPU-only `src/native/usd` library depends on
OpenUSD, JSON, and the CPU scene library, with no SDL, Vulkan, or dxvk dependency.

The standalone sample accepts an existing capture or project:

```sh
remix-native-sample /path/to/libremix.so 120 pathtrace /path/to/capture.usd
```

A frame count of zero runs until the window closes. The sample uses the stage's
camera, units and up axis, and disables the game-oriented sun/range overrides.
Its mod host coordinates are obtained from the capture metadata.

## Independent versioned extensions

`remix_usd_ext.h` exports load, draw, asynchronous reload, destroy, and diagnostics
functions. Initialize load/draw structs with `structSize=sizeof(struct)` and
`version=1`. Loading returns a scene handle without requiring a Vulkan device;
resource submission occurs on draw. Draw on the startup/render thread before
Present, as with ordinary API scene submission. Reload parses USD and decodes texture assets on a background
thread and publishes complete resources before the next scene submission at a
frame boundary. Failure preserves the previously usable snapshot. Diagnostics
report counts, coordinates, warnings, generation, and the last reload error;
handle zero reports a failed load on the calling thread. Destroy scene handles
when finished. Shutdown releases submissions; a later startup can recreate them
from retained CPU snapshots.

Host units and up axis are explicit options. Zero retains stage metadata. Private
layer copies anchor references, sublayers and texture assets to their authoring
layers, normalize Windows separators, and never modify source files. Composition
covers mesh instances, material subsets, indexed primvars, legacy alpha state,
category metadata, captured skeletal poses, lights and cameras. Interactive
editing continues through the toolkit's existing USD authoring/snapshot path.
Animation playback, arbitrary MDL execution, new capture generation, Direct3D
injection, and game-client changes are outside this integration.

Captured alpha blending uses coverage: zero-alpha texels leave the background
unchanged, regardless of RGB. Empty coverage is rejected before shading and does
not consume the transparent-layer budget. Exact alpha-one texels use opaque
lighting and their own motion/NRD guides, even when their draw enables blending.
Primary fractional coverage honors the configured local-light sample count.

The four decal categories identify paint layers when blending is enabled.
Category-tagged opaque walls retain ordinary surface/shadow behavior. The path
tracer blends a paint layer's albedo, mapped normal, squared roughness, metallic
value and emission into an opaque receiver before evaluating lighting. Primary
and indirect hits use the same material composition; decal geometry does not
cast a separate sheet shadow.
The receiver supplies geometry/motion/NRD guides, so partial paint coverage is
filtered with the receiver's lighting instead of composited as unfiltered light.
Constant opacity remains paint coverage; explicit translucent glass keeps the
native thin-sheet model. Unattached decals and decals over translucent receivers
fall back to ordinary coverage transport.

Decals resolve in geometric front-to-back order, with a 32-layer bound. This is
not dxvk-remix's unordered, draw-order decal binning, and does not add its coplanar
offset/priority system. The decal resolver is specialized out when a scene has no
paint. Stable depth used for raster gameplay feedback still excludes blended
draws and paint layers. Other fractional-alpha surfaces retain source-over
composition after NRD; their own lighting is not separately NRD-filtered.
Untagged authored constant-opacity materials and explicit glass retain the
colored thin-sheet approximation, including attenuation at zero opacity.
Reconstruction receives settling frames after each scene change before static
output is reused, so an opacity edit cannot freeze partially resolved FSR color.

## Hash replacements and reload

The native API discovers sorted `mod.usd`, `mod.usda`, and `mod.usdc` paths beneath
`rtx-remix/mods` in the host working directory, or `OPENREMIX_MODS_DIRECTORY`.
`remixapi_ConfigureUSDModsEXT` selects the directory, watch behavior, and explicit
host coordinates. Without explicit configuration the host convention is Y-up,
centimeters. The first matching mesh, material or light hash wins. Direct capture
replay supplies the hashes stored in `mesh_`, `inst_`, `mat_`, and `light_` names.

Original resources remain separate from replacements. Preserve-original flags,
material-only changes, hidden/deleted entries, and parent-relative attachments
are supported. Attachments derive identities from the live parent identity and
USD path; batched instances include their instance index. Clients without stable
identities use a transform-derived fallback and do not retain correspondence
across transform changes. Lights attached to disappearing parents are retired.
Property-only light overrides merge with the original light definition.

Layer and asset modification times and sizes are checked at 100 ms intervals;
changes are debounced for 250 ms. USD parsing and texture decoding run off the render thread; complete
sets publish after Present. Missing, malformed, or undecodable replacements keep
the previous working set and report a diagnostic. Removing a mod restores the
original resources. Texture contents are reloaded even when their path is unchanged.

## Embedded rendering

`remix_editor_ext.h` provides startup flags for headless operation, disabled game
profiles, and disabled automatic mods. The toolkit uses all three flags: it already
submits a composed scene and maintains its own preferences. OpenRemix settings use
only `rtx.native.*`; the worker queries native backend/reconstruction capabilities
and reports effective fallbacks separately from saved preferences.

On Linux `remixapi_EditorConfigureSharedFrame` exports the version 1 shared-frame
contract: RGBA8 UNORM, dedicated opaque-FD Vulkan memory, binary ready/released
semaphores, device UUID and allocation metadata. No SDL window, Vulkan surface,
or swapchain presentation is created. The renderer releases image ownership to
`VK_QUEUE_FAMILY_EXTERNAL` in GENERAL layout and waits for the consumer's release
before the next frame. Present succeeds only after its submission fence completes,
so a successful worker frame acknowledgment means the image is ready for import.
The consumer must validate the device UUID and honor the semaphore/ownership cycle.

## Lighting and materials

USD and API lights share the same internal conversion: color, blackbody temperature,
intensity/exposure, dimensions, world transform, and cone/focus shaping. Sphere/spot,
rectangle, disk, cylinder, distant, and dome lights participate in path tracing.
Rectangles use two triangles; disks and cylindrical sides use 64 angular segments.
Area samples use the existing direct/indirect samplers and visibility rays. A
textured, rotatable lat-long dome illuminates surfaces, including at zero diffuse
bounces. Raster is an unlit material preview. OpenRemix's material approximations
remain in effect; output is not promised to match dxvk-remix pixel for pixel.

DDS BC4 scalar maps, BC6H signed/unsigned HDR, and half/full-float DDS plus Radiance
HDR images retain linear floating-point values through GPU upload. The optional
`MaterialNormalEncodingEXT` distinguishes hemispherical-octahedral, tangent DX,
and tangent GL maps without changing legacy clients' automatic XY/XYZ decoding.
The retained Remix 0.1003 interface table and base structure layouts are unchanged.
