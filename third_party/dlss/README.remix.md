# NVIDIA DLSS SDK 310.9.1

Unmodified NGX headers, Linux x86-64 and Windows x86-64 release libraries from
[NVIDIA/DLSS](https://github.com/NVIDIA/DLSS/tree/374959484e79a640feaba44c93ac8cfb0a03f5b5),
revision `374959484e79a640feaba44c93ac8cfb0a03f5b5`.
`sources.sha256` records the downloaded artifacts; their Git blob hashes were
checked against that revision's tree. `LICENSE.txt` is the upstream license.
Frame-generation and development libraries are intentionally excluded.

Linux links `libnvsdk_ngx.a`; native Windows clang-cl links `nvsdk_ngx_d.lib`
(the dynamic CRT variant). These are dispatch libraries, with no load-time NGX
runtime dependency. The renderer queries both features before creating the
Vulkan device, then explicitly searches the adjacent `dlss/310.9.1` directory.
Meson development builds use the adjacent `upscale` artifact directory. Both
paths require matching packaged release files before enabling the feature.

SR uses model preset K and RR uses model preset E. The other new default models
in this release (SR L/M and RR F) produced internal Vulkan validation errors on
the tested Turing Linux driver. Quality modes and dimensions still come from
NGX's optimal-settings callbacks. Native Windows RTX validation remains a
separate release gate.
