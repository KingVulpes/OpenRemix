> Historical findings from the removed renderer, retained for driver investigations.
> Build and runtime instructions below describe that historical revision only.

# Blocking macOS graphics failure — 2026-09-07

**Status: the native sample now completes 300 frames with a visible path-traced
scene, NRD/TAAU, and no validation errors. RTXDI remains a compatibility limit.**

An earlier 300-frame attempt on Apple M5 was followed by an unresponsive
WindowServer and a host reboot. The user subsequently authorized continued GPU
testing. The run scripts retain explicit experimental GPU opt-in, and root
Meson GPU tests default to disabled on macOS. The subsequent successful runs do
not prove that every cause of the original graphics failure has been resolved.

## Environment and observed sequence

- Apple M5, arm64, macOS 27 beta build `26A5425a`, Xcode beta / Apple Clang 21.
- Vulkan SDK 1.4.350.1, Slang 2026.14, pinned experimental MoltenVK and
  SPIRV-Cross from `scripts-macos/dependencies.lock.json`.
- Both tracked MoltenVK patches applied. Vulkan validation and Metal API
  validation enabled; experimental ray tracing enabled.
- Run started around 10:57 local time with 1280×720, NRD enabled, a requested
  300 frames, and a final presented-image screenshot. The 300-frame screenshot
  was not produced.
- At 10:59:08 CDT, the WindowServer watchdog reported 40 seconds without a
  successful check-in and terminated it.
- At 10:59:48 CDT, the watchdog reported 80 seconds without a successful
  check-in; the restarted WindowServer had not finished initialization after
  `IOKitWaitQuiet`.
- The user reported a full reboot. Post-reboot uptime confirmed a recent boot.
  No readable kernel panic report was identified in the inspected directory.

The saved renderer log reports
`VUID-vkAcquireNextImageKHR-semaphore-01779`: reuse of an acquire semaphore with
pending operations. It also contains an image-format warning for the unused
Ray Reconstruction hit-distance binding. The watchdog reports establish a
system graphics failure during this workload, **not** which layer caused it.
Remix synchronization, experimental MoltenVK, and the beta Apple graphics stack
remain possible contributors. Validation errors must be resolved before
attributing the reboot solely to a driver defect.

## Evidence retained locally

- `_run-macos-300/sample.log` and `remix-native.log`: failing run.
- `/Library/Logs/DiagnosticReports/WindowServer-2026-09-07-105908.ips`.
- `/Library/Logs/DiagnosticReports/WindowServer-2026-09-07-105948.ips`.
- Corresponding `WindowServer_...userspace_watchdog_timeout.spin` files.
- `_deps-macos/probe-semaphore-unpatched.log`: isolated binary semaphore hang.
- `_deps-macos/probe-semaphore-patched.log`: nine probe tests passed.
- `_deps-macos/bootstrap-tests.log`: eleven presentation/CPU/ABI tests passed
  before the reboot (including the presentation-only Vulkan smoke test).
- `_run-macos-sixth/sample.png`: earlier three-frame capture showing the welcome
  overlay on black. This is **not** evidence of correct scene path tracing.

Keep system diagnostic reports local: they may contain unrelated process and
machine information. A sanitized extract is sufficient for an upstream report.

## Implementation and verification boundary

The arm64 full runtime and native dependency bundle built successfully before
the failure. The isolated probes verified ray query and ray pipeline hit/miss
readback, TLAS updates, nonzero AS/buffer offsets, large bindless tables, and
binary semaphore behavior after a small driver patch. A real Remix G-buffer
compute pipeline was created successfully. These checks do not establish
correct full-frame rendering.

A subsequent change serializes the macOS present queue before the presenter's
acquire-semaphore ring is reused. Another supplies an R16F resource for the unused
hit-distance binding. The release runtime was rebuilt successfully. With user
authorization, repeated 6-, 8-, 12-, and 30-frame runs completed and shut down
without Vulkan validation errors or another observed host reboot. This does not
establish that the original system failure is fixed.

Post-reboot verification: 10 CPU/ABI tests and 9 standalone Vulkan/SPIR-V probes
passed. At that stage, captures remained black behind the splash: every primary linear-depth
pixel in the debug capture is the miss sentinel (500001), and normals, albedo,
and lighting outputs are zero. CPU camera matrices are consistent with the
sample, the camera is valid, and scene surface data is present. Disabling
asynchronous compilation or BLAS merging did not change this result. A separate
G-buffer TraceRay-mode experiment exceeded its 40-second process timeout; the
normal compute ray-query path completed short runs.

Evidence: `_run-macos-resumed-30`, `_run-macos-debug-capture`,
`_run-macos-unmerged-config`, `_run-macos-camera-trace`, and
`_run-macos-traceray`; `_deps-macos/resumed-cpu-tests.log` and
`_deps-macos/resumed-probe.log`. Those captures were investigated further as described below. They should not
be used to assess the corrected renderer.

## Scene-query isolation

Temporary G-buffer diagnostics queried the runtime's actual scene TLAS, using
the same output resource and camera constants as the renderer. A fixed ray from
(0, 0.1, -4) toward (0, -0.05, 0.998749) hit the sample triangle at 5.00626.
In a split capture, all 115,200 fixed-ray pixels hit; camera-generated rays in
the other half produced 17,843 hits with distances up to 9.44919. Evidence:
`_run-macos-scene-query` and `_run-macos-camera-query`. These are diagnostic
outputs, not final rendered images. They narrow the black-scene investigation
to the full G-buffer query/resolution path; basic geometry and camera ray
traversal work. Explicit resolver-state initialization and disabling face
culling did not restore the full image.

## Resolved black-scene causes and current results

1. The native sample left `MaterialInfoOpaqueEXT.alphaTestType` at zero, which
   means Never. A diagnostic immediately before material processing saw 35,729
   valid hits; a diagnostic after material processing classified every one as
   rejected transparency. Setting the sample's alpha test to Always restored
   geometry, normals, and albedo. This was a sample bug, not a driver defect.
2. RTXDI still produced zero direct diffuse radiance. Disabling it restored
   lit geometry and shadows through ordinary direct light sampling. MoltenVK
   initialization now supplies that default; an explicit configuration override
   can re-enable RTXDI for investigation. Its precise failure layer has not
   been isolated, so it is not attributed solely to MoltenVK.
3. NGX-free runtime initialization now falls back from DLSS to TAAU. All temporary
   camera traces and diagnostic shader changes were removed.

`_run-macos-final-300/result.json` records 300 completed frames, NRD enabled,
1280×720 output capture, exit 0, no validation errors, and 8.07 seconds total
wall time. `_run-macos-final-raw-300` records the corresponding NRD-disabled run
in 6.02 seconds. Both images show the triangle, ground plane, and cast shadow.
Ten CPU/ABI tests passed after the final source rebuild. This validates the
bounded native sample with the compatibility fallback, not arbitrary Remix
content or production macOS stability.

The bundled runtime additionally passed the 480-frame V-sync transition test
(23.28 seconds, Vulkan/Metal validation, valid capture, no validation errors).
Dynamic-loader tracing confirmed all four bundled graphics/runtime libraries
were loaded from `_package-macos/lib`. Evidence is in `_run-macos-packaged`.
No further host reboot was observed during the resumed testing. Interactive
resize/minimize and arbitrary game content remain outside this verification.
