# Shader toolchain

Normal Linux, Windows, macOS and Android builds use checked-in SPIR-V. No shader
compiler, network access, NRI runtime or external NRD installation is required.
`-Dnrd=false` omits NRD; TAA and the raw tracer remain built.

The diffuse SH shader identifiers are pinned to NRD 4.18.0 revision
`fca9486957dececb7a7d5b5646d0829c667a01cf`. The integration follows NRD-Sample simplex
`6d9aa308d5f4a93cf6c0f7d10fce873c9c8c4c25`. Dependency revisions, provenance and
licenses are in `third_party/nrd/README.remix.md`; the ABI audit verifies vendored
file hashes. Upstream `NRD.hlsli` is included directly in its GLSL compatibility mode.

For regeneration on Linux x64, install glslang and SPIR-V Tools, then fetch the
optional pinned DXC release (Python 3.12+). The helper verifies the archive SHA-256
and compiler version and requires a fresh destination:

```sh
python3 scripts/shaders/fetch-dxc.py --output _work/deps/dxc-shaders
export REMIX_DXC="$PWD/_work/deps/dxc-shaders/bin/dxc"
python3 scripts/shaders/generate.py all
python3 scripts/shaders/generate.py all --check
python3 scripts/shaders/generate.py all --validate
```

Select `raster`, `pathtrace`, `denoise`, `nrd`, `upscale` or `fsr31` for a focused update. `--check`
regenerates without changing files; `--validate` only needs `spirv-val`. Generated
output depends on compiler versions; this revision was verified with glslang 16.4.0,
SPIR-V Tools 2026.3 and DXC 1.9.0.5191. See `toolchain.lock.json` for DXC's release,
download URL and checksum.

Only diffuse SH, clear and validation permutations are embedded. Hit-distance
reconstruction, checkerboarding, viewport offsets, performance variants and quad
intrinsics are disabled. History confidence and anti-firefly support are enabled.
NRD's reported identifiers and binding offsets select pipelines and descriptors;
an unexpected dispatch fails explicitly. CPU NRD is a static library with hidden
symbols and all upstream embedded backends disabled.

DXC emits some unknown-format float3 image writes against RGBA16F SH resources.
`storage_writes.py` deterministically pads those SPIR-V texels to four components
before validation, satisfying Vulkan's storage-write component-count requirement
(VUID-RuntimeSpirv-OpImageWrite-08795). The unused component is zero; upstream HLSL
and RGB lighting semantics are unchanged.

FSR 3.1.4 uses the pinned GLSL SDK sources under `third_party/fsr31`. The `fsr31`
target compiles the supported HDR/FP32 permutations and reflects descriptor names,
bindings, array counts and sets from SPIR-V. Both reproduction and SPIR-V validation
are part of `all`. The `upscale` target builds input preparation and display
composition. Normal builds require no FidelityFX shader compiler or runtime DLL.
