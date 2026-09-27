# Path-traced exposure and bloom

Path tracing meters unexposed HDR world color after reconstruction, then applies
threshold-free bloom and exposure before Prism and output encoding. The HUD,
F12 panel, viewmodels and subsequent raster effects keep their existing rendering
path. Reconstruction and denoising continue to receive unexposed radiance.
Diagnostic views and validation overlays bypass both effects and manual exposure.

Both effects default on, including when loading an older profile that omits their
keys. Profiles save requested values even if resource preparation fails. F12's
post-processing tab exposes the controls, GPU time, allocated memory and metered
exposure. These are internal renderer features; the Remix 0.1003 ABI is unchanged.

All keys below start with `rtx.native.pathtrace.`:

| Key | Default | Range |
| --- | --- | --- |
| `autoExposure` | true | Boolean |
| `exposure` | 1 | 0.05–8 |
| `autoExposureTarget` | 0.2 | 0.02–0.6 |
| `autoExposureSpeed` | 2 | 0.1–16 |
| `autoExposureCentre` | 0.75 | 0–1 |
| `autoExposureMinEv` | −16 | −24–0 |
| `autoExposureMaxEv` | 2 | 0–24 |
| `bloom` | true | Boolean |
| `bloomIntensity` | 0.1 | 0–1 |
| `bloomRadius` | 0.85 | 0–1 |

Manual exposure multiplies the automatic exposure. The default maximum of +2 EV
limits night brightening to four times the original radiance. Automatic exposure
uses the geometric mean of average RGB, weighted toward an aspect-correct circular
center. Each sample compensates for surface color with
`radiance * 0.18 / max(materialColor, 0.05)` and a brightness floor of `1e-4`.
The dedicated material guide contains base color, independent of NRD material
factors. Transparent layers use the same front-to-back weights as radiance; sky
uses white. Existing visibility and shading supply the guide without extra rays.
Nonfinite samples are ignored. An entirely invalid frame retains the previous
exposure, or unity before the first valid sample.

The metering algorithm follows the Halo reference's `composite.hlsl` under
`reference/postfx/halocedlaart6`. A 16×16 reduction writes partial sums, followed by
one reduction/adaptation dispatch. Linear exposure adapts with
`1 - exp(-min(dt, 0.25) * rate)`. The rate is in inverse seconds, not stops per
second. First use, re-enablement and explicit world/camera resets snap to the new
target. Resize, VSync and same-world tracer replacement preserve GPU exposure.
Retained loading frames freeze adaptation and discard elapsed loading time.

Bloom follows the 13-sample downsample and 9-sample tent reconstruction in
`reference/postfx/bloom.md`. Downsampling uses black borders; reconstruction
clamps at edges and blends each level with the next smaller reconstructed level.
The pyramid starts at half resolution, with
`clamp(floor(log2(min(width,height)/4)), 1, 12)` levels. Every dimension has at
least one texel. Downsample and reconstruction images have distinct storage.
There is no threshold or additional Gaussian pass. Final composition is
`mix(sceneColor, bloomColor, bloomIntensity) * exposure`.

Post-processing resources prepare independently on the existing pipeline worker.
Scalar edits and disabling effects keep the tracer and its lighting history.
Compatible cached pipelines survive toggles and resizing. Activation uses the
existing safe frame boundary; failed preparation preserves the working
configuration and reports the error in F12. Before a working configuration
exists, composition uses unity exposure and disabled bloom. Cached HDR can
continue adapting without retracing, and bloom is reused until HDR or spread
changes. Diagnostics read completed frame slots without adding a GPU wait.
