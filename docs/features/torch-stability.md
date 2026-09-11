# Torch sampling and temporal stability

Local-light samples retain the analytic sphere transport model and one visibility
ray per requested sample. Russian roulette is not used. Radiance is not clamped
in the light estimator; the existing output finite-value/FP16 guard remains.

## Small light sets

When the submitted LightBVH contains two to four sphere-light slots, the sampler
generates one solid-angle candidate for each light. A luminance-weighted reservoir
selects a candidate, followed by one fresh visibility query. If the candidates have
physical contributions `C_i` and probabilities `p_i`, the returned sample is
`C_selected * visibility / p_selected`. Conditional expectation over reservoir
selection gives the sum of candidate contributions after visibility. Averaging
over sphere directions gives the original sphere integral.

The previous-frame visibility guide may select its preferred light as a separate
proposal. The denominator includes both proposals, evaluated for the realized
candidate set. If the preferred candidate has no contribution, the sampler uses
the base distribution. Different light colors remain separate RGB contributions;
selection uses luminance, without replacing RGB by luminance or dropping weak
emitters. This targets brightness variance; differently colored lights can still
have higher variance in individual color channels. Zero-power slots never
contribute.

This reduces discrete light-selection variance without tracing every light's
shadow. It costs extra candidate arithmetic for small sets. Single-light and
larger sets retain LightBVH traversal and visibility guidance. Lazy pipeline
specialization removes the small-set reservoir from their shaders, including when
local illumination is disabled. Variants follow the original pipeline's fence and destruction lifetime;
initialization failure retains the ordinary unbiased LightBVH estimator.

## Temporal sampling and filtering

Diffuse directions and lighting-confidence retracing retain the existing random
sequence and PDF. No new history buffer, longer history window, or extra pass is
introduced. Public interfaces and the private CPU/GPU layout size are unchanged.

REBLUR already enables anti-firefly processing. While local lights are active,
the diffuse prepass is disabled for both direct local radiance and indirect SH.
Discrete light selection produces inverse-PDF weighted samples; pre-accumulation
spatial reuse can expand an isolated bright sample into a blob before NRD's
anti-firefly stage. This follows the pinned `ReblurSettings` guidance for
probabilistic signals. NRD's subsequent temporal and spatial filtering remains
active. Sky-only indirect lighting retains its existing prepass.

While local lights are active,
the existing firefly-relative scale and fast-history clamping scale use 1.5.
This applies to direct local radiance and indirect SH. It increases outlier
rejection without enlarging spatial blur radii or lengthening history. Like other
denoiser clamping, it can alter high-contrast details; reference comparisons must
check energy as well as noise. Sun SIGMA and the TAA algorithm are unchanged.

Water's own lighting still uses the transparent composition and TAA path. Its
local-light estimator benefits from small-set resampling; it is not incorrectly
fed to NRD with the opaque background's depth and motion guides.

## References

- *Ray Tracing Gems I*, chapter 18: importance sampling of many lights.
- *Ray Tracing Gems II*, chapters 22–23: weighted reservoirs and light candidates.
- *Ray Tracing Gems II*, chapters 24–25: sampling noise and reliable temporal reuse.
- Local `NRD-Sample/Shaders/TraceOpaque.cs.hlsl`: preserve estimator quality before
  denoising, keep confidence retracing consistent, and treat roulette cautiously.
- NRD's input/sampling guidance and `ReblurSettings`: short-history convergence,
  anti-firefly processing, and fast-history clamping.
