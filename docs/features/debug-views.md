# Live path-tracing diagnostics

F12 → **Diagnostics** → **Debug view** exposes the inputs needed to distinguish
visibility, shading, and temporal artifacts. The default is **Final lighting**.
The same selector is available as `rtx.native.pathtrace.debugView` (0–11).

| Value | View | Interpretation |
| --- | --- | --- |
| 0 | Final lighting | Normal rendered output. |
| 1 | Albedo | Unlit base color, before illumination. |
| 2 | Geometric normals | World XYZ mapped to RGB in 0–1. A flat face has a constant color. |
| 3 | Triangle IDs | Hashed persistent instance/surface/triangle identity. Remeshing can change colors; the finite palette permits collisions. Legacy submissions use their submission identity. |
| 4 | Depth bands | Camera distance repeating black to white every four world units. |
| 5 | World position grid | World XYZ as RGB, repeating every world unit. |
| 6 | Raw local lighting | Direct torch and emissive illumination, before albedo and transparent tint. Includes both families in separate modes. |
| 7 | Raw indirect lighting | Indirect illumination before albedo and transparent tint, including SHARC if active. Zero bounces produces black. |
| 8 | Motion vectors | Opaque guide motion in internal pixels: red is X, green is Y, gray is zero, full range is ±16 pixels. Magenta means invalid correspondence. |
| 9 | Motion validity | Green indicates available correspondence; red indicates invalid motion or reset. This precedes depth/normal rejection and is not a denoiser acceptance mask. |
| 10 | Indirect lighting confidence | The actual REBLUR diffuse SH confidence input: white is high confidence. It does not describe the local-light filter. |
| 11 | TAA current-frame weight | Actual native TAA blend weight after rejection/clipping: white uses only this frame, darker colors retain more history. Unavailable with FSR. |

Geometry views display the first visible surface, including transparent sheets.
Lighting and temporal guides describe the opaque receiver behind those sheets.
A purple checkerboard means the requested signal is unavailable in the active
pipeline. No extra temporal resources are enabled to synthesize missing data.
False-color and albedo views bypass Prism and custom gamma; raw lighting uses the
selected display transform. All views retain normal output encoding.

Start with Triangle IDs, normals, and the position grid at the problem angle.
Compare **Primary visibility** between Rasterized G-buffer and Ray traced.
Changes in those views can indicate unstable surface visibility or geometry;
changes confined to raw lighting or temporal guides point further along the
rendering path. Raw sampling noise and ordinary subpixel edge jitter remain
visible: these diagnostics deliberately display each current frame.

Diagnostic selection changes only presentation. NRD, native TAA, SHARC, and FSR
continue processing the normal scene with the requested settings. Their histories
are not reset by changing this selector. FSR diagnostic pixels are enlarged with
nearest sampling from the internal resolution, while normal FSR history keeps
running. Switching back restores the regular presentation. Progressive reference
accumulation is invalidated on selector changes and bypassed in diagnostic output.
Debug views force fresh world frames, including stationary scenes; timings while
inspecting them therefore need not match a warmed, cached final view.

The implementation reuses reserved private frame words and existing scratch
images. Triangle identity occupies the unused alpha channel of the raster
emission guide and does not affect visible emission. Opaque diagnostics travel in
background scratch RGB through transparent composition. Final temporal composition
preserves this display payload or visualizes its own guides after updating the
normal history. No public structures, exports, or additional image allocations
are required.
