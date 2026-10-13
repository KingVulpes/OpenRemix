# Sparse rendering

`rtx.native.pathtrace.sparseRendering` (0/1, default 0; the Rendering tab's
**Sparse rendering** checkbox, shown with DLSS Ray Reconstruction) lights only
part of the pixels each frame and leaves the rest to Ray Reconstruction. It
follows dxvk-remix's `rtx.sparseRendering`, including its rate scaling.

## What is rendered

- A blue-noise threshold selects the frame's pixels: evenly spread over the
  screen, and per pixel evenly over 32 frames.
- Selected pixels get the whole opaque lighting pass (sun, local and emissive
  lights, the indirect path), at 1 / rate times their radiance. The others get
  none.
- Guides (albedo, normal, roughness, depth, motion), emission, sky and
  transparent layers are computed at every pixel, as without sparse rendering.

The path tracer's other passes are unchanged. Sparse rendering needs DLSS Ray
Reconstruction and stays off with ReSTIR, a separate local-lighting or indirect
pass, local-light reuse, accumulation and debug views. The timings dump says
which (`# sparse rendering: ...`), as does the Rendering tab.

## Sampling rate

The rate is the share of the pixels lit each frame.

| Setting (`rtx.native.pathtrace.`) | Default | |
|---|---|---|
| `sparseAdaptiveRate` | 1 | Scale the rate with the render resolution. 0: `sparseSamplingRate` as is. |
| `sparseSamplingRate` | 0.357 | The rate at 2227x1238 (4K output, DLSS Auto). |
| `sparseResolutionExponent` | 0.69 | rate = `sparseSamplingRate` x (2227x1238 / render pixels) ^ exponent. 0: the same rate everywhere; 1: the same number of lit pixels. |
| `sparseQuality` | 0.5 | Moves the scaled rate, in log space, towards 1/16 (0) or 1 (1). |
| `sparseMaxRate` | 0.75 | Above this rate sparse rendering is off. |

A lower render resolution leaves Ray Reconstruction less to rebuild from, so
it takes a higher rate, and below roughly 1280x720 the defaults turn sparse
rendering off. dxvk-remix's graphics presets pair the rate and exponent:
Ultra 0.357 / 0.69 (the defaults), High 0.228 / 0.83, Medium 0.125 / 0.83,
Low 0.1 / 0.91.

## How it runs

`pathtrace/sparse.cpp`, `sparseSelected` and the two passes in `pathtrace.comp`.

1. **Select and list** (`sparseCompact`): one group per 16x16 tile marks its
   selected pixels, claims a run in the pixel list and writes them in Z order.
   A second one-invocation pass (`sparseArgs`) publishes the count as an
   indirect dispatch.
2. **Guides of unlit pixels**: the opaque shader specialized without lighting
   (`sparseLaunch` 2), over the screen; selected pixels return at once.
3. **Lighting of selected pixels**: the opaque shader (`sparseLaunch` 1)
   dispatched indirectly over the list, so its groups are full of work instead
   of mostly idle.

The two opaque variants compile on first use; until then the frame is lit
densely.
