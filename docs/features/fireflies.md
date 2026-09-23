# Local-light firefly suppression

Three opt-in settings control bright local-light samples and continuation paths.
All shipping defaults remain disabled. F12 exposes numeric clamp fields and an
independent **PBR local-light firefly filter (REBLUR)** checkbox.

| `rtx.native.pathtrace.` key | Default | Meaning |
| --- | --- | --- |
| `localDirectClamp` | `0` | Maximum outgoing luminance per completed primary local-light sample |
| `indirectClamp` | `0` | Maximum outgoing luminance per completed continuation path |
| `localFireflyFilter` | `false` | Keep local PBR radiance out of REBLUR pre-blur |

Clamp thresholds accept finite numbers from 0 to 1,000,000, in linear scene
luminance. Zero bypasses clamping. Lower thresholds suppress more brightness and
can remove legitimate glossy highlights and reflected energy. There is no
recommended nonzero shipping threshold. Compare baseline, clamp-only,
filter-only and combined settings against converged unclamped HDR references.

Direct clamping covers both torch spheres and emissive triangles, including
transparent-sheet shading. Each sample includes visibility, reservoir weighting,
and light-family probability compensation before clamping. Per-family sampling
uses a single scale for the combined torch and emissive contribution at each
iteration. Averaging follows clamping, so sample counts do not rescale the limit.
Primary sunlight and directly visible emission remain unchanged.

Indirect clamping applies once to the completed continuation path, including
local lights, sunlight, the sky and SHARC tails. SHARC training remains unclamped.
Confidence retracing uses the same limit as the stored indirect comparison
signal. Thresholds measure actual outgoing RGB by restoring the material factor;
the resulting luminance scale is shared by total and specular radiance. This
preserves RGB ratios and diffuse/specular separation. Sampling probabilities,
random draws, ray budgets and hit distances are unchanged.

Either clamp bypasses experimental screen-space PBR lighting reuse. Indirect
clamping also bypasses water irradiance sharing. Saved reuse settings resume when
the relevant clamps are disabled; bypassing reuse can increase GPU time. Changing
any of the three controls resets temporal histories and progressive accumulation.

The independent REBLUR experiment disables diffuse radiance pre-blur for the PBR
pass when local lighting enters that denoiser, either directly or through
continuation paths. The specular prepass remains enabled
for motion estimation using `usePrepassOnlyForSpecularMotionEstimation`. Existing
anti-firefly settings, history limits, subsequent filtering and hit-distance
reconstruction are retained. The UI identifies the filter as inactive when REBLUR
is bypassed, including progressive accumulation and DLSS Ray Reconstruction.
Clamps operate with raw output, REBLUR, TAA and reconstruction modes.

Implementation uses private renderer settings and frame data. Remix 0.1003 public
layouts, exports and BetaRT's JNI protocol are unchanged. BetaRT launch variables
are `MCRTX_PT_LOCAL_DIRECT_CLAMP`, `MCRTX_PT_INDIRECT_CLAMP` and
`MCRTX_PT_LOCAL_FIREFLY_FILTER`.

The luminance scale follows [RTX Remix's contribution filter](https://github.com/NVIDIAGameWorks/dxvk-remix/blob/main/src/dxvk/shaders/rtx/utility/common.slangh).
The bias/highlight tradeoff is also described in [Blender's noise guidance](https://docs.blender.org/manual/en/4.3/render/cycles/optimizations/reducing_noise.html).
NRD documents the motion-only prepass option in its vendored README and
`ReblurSettings` declaration.

## Opt-in diagnostics

`REMIX_PATH_LINEAR_CAPTURE=/absolute/path/frame.pfm` reads back linear RGB before
the display transform. It captures the native world lighting image, including
REBLUR/TAA when active; it is not the DLSS/FSR reconstructed output. The PFM uses
little-endian floats and bottom-to-top rows. Set the variable for one submitted
frame, then unset it and submit additional frames to retire that frame's fence.
Capture buffers are private to each frame slot. Optional
`REMIX_PATH_LINEAR_SIGNAL=diffuse` or `specular` captures the corresponding PBR
REBLUR input, decoded from half precision YCoCg to linear RGB; these inputs retain
their separate material demodulation factors. Capturing adds a GPU copy and CPU
file I/O; keep it disabled for timing.

`REMIX_PATH_SAMPLE_SEED` adds a diagnostic offset to the transport sequence for
independently seeded references. With the variable unset the original sequence is
retained. These diagnostics have no NEE-cache or roulette dependency.

Build the GPU fixture with `-Dgpu_tests=true`. The optional
`scripts/experiments/fireflies.py` driver runs regression matrices, reference and
threshold sweeps, or three alternating baseline/candidate timing pairs per mode.
Its `--output` directory records exact settings, HDR captures and measurements.
Timing uses ten seconds of warmup and twenty seconds of measurement per run.
