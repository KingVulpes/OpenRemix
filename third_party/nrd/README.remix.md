# Native diffuse SH dependencies

NRD 4.18.0: https://github.com/NVIDIA-RTX/NRD/tree/fca9486957dececb7a7d5b5646d0829c667a01cf
MathLib v11: https://github.com/NVIDIA-RTX/MathLib/tree/974e1387ba936740c7cdc494792d2641bc127e86
ShaderMake headers: https://github.com/NVIDIA-RTX/ShaderMake/tree/ca92f65f355f0e31cb67a9601baec705441f6401
sse2neon v1.8.0: https://github.com/DLTcollab/sse2neon/tree/3cf69760cc6fdc45a5d70f0d42a8f079489f9867

Integration reference: NRD-Sample simplex revision
https://github.com/NVIDIA-RTX/NRD-Sample/tree/6d9aa308d5f4a93cf6c0f7d10fce873c9c8c4c25

Each dependency retains its license alongside its sources. Upstream files are
unchanged. NRDConfig.hlsli is our build configuration. Meson builds the CPU library
with hidden symbols, no embedded shader backend, and no NRI/ShaderMake runtime.
Our Vulkan adapter maps dispatch identifiers to checked-in SPIR-V.
