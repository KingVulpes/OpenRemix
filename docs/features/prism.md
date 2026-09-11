# Prism SDR display transform

Path-traced world output defaults to the standalone Prism tonemapper supplied in
`ref_tonemap/prism/prismTM.hlsl`: D65 / BT.709 input, supplied model primaries,
0.18 anchor, compression 4 and SDR peak 1. The constant inset and inverse matrices
are precomputed; the anchored C-infinity shoulder runs in the inset basis. Final
out-of-gamut RGB is clipped to the SDR display range. The shader retains the
reference's MIT notice. The separate RenoDX primary-authoring UI and grading
controls are not part of this port.

Prism runs once in the existing final world composite, after linear-light
accumulation, NRD, TAA and FSR (including sharpening). It adds no image, sampling
pass or temporal state. Sky and transparent layers already composed into world
color receive the same transform. Subsequent raster effects and UI retain their
existing output path.

F12 exposes **Prism tonemapping** and **Gamma 2.2 output**. The corresponding
`rtx.native.pathtrace.prism` and `rtx.native.pathtrace.gamma22` configuration keys
accept booleans. Both default on. Disabling Prism restores the original clipping;
disabling Gamma 2.2 restores the original sRGB transfer. Display changes do not
invalidate linear lighting history. Explicitly disabled output conversion still
returns linear values.

Gamma 2.2 encodes display-linear RGB as `pow(rgb, 1/2.2)`. For an sRGB attachment,
the shader decodes that result with the exact sRGB transfer before the attachment
encodes it, avoiding double encoding. A UNORM fallback receives the encoded value
directly. Texture decoding and all lighting calculations remain linear.
