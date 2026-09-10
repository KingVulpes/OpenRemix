# Remix 0.1003 native API

The public SDK declarations are retained byte-for-byte. Some header comments describe
the removed renderer; this document defines current behavior. The ABI baseline in
`tests/abi` records public-header hashes, every field offset and size of all 50 public
structs on POSIX and Windows, and the 106 pre-removal POSIX exports.

| API area | Runtime behavior |
| --- | --- |
| InitializeLibrary | Checks ABI compatibility and fills every platform interface slot. Does not open a window. |
| Startup / Shutdown | Creates/destroys SDL window, Vulkan resources and private ImGui context. Repeated shutdown succeeds. Duplicate startup reports ALREADY_EXISTS. Failed startup cleans up. |
| Present | Clears, draws submitted geometry, tint/overlay, game UI, then debug UI. Null PresentInfo is accepted. Minimized/unavailable surfaces skip GPU work and discard that frame's geometry/UI. |
| Window/input | SDL3 event pumping, window state, keyboard, mouse, capture, cursor, fullscreen and high-DPI drawable sizes. |
| UI state | NONE hides the panel; BASIC and ADVANCED show the same panel. Initially NONE. |
| Feature queries | NRD reports its build/device capability independently of backend selection. DLSS/upscaling and other removed features remain unavailable; frame-generation limit is zero. |
| Config | After startup, `rtx.native.vsync` accepts `true`, `false`, `1`, `0`. `rtx.native.backend` selects `raster` or `pathtrace`; `rtx.native.pathtrace.primaryVisibility/samples/bounces/accumulate/forceRebuild/denoiser/taa/validation` control tracing. Unsupported tracing returns FEATURE_UNAVAILABLE and keeps raster. Other `rtx.*` keys are accepted and ignored. Unknown namespaces report FEATURE_UNAVAILABLE. |
| Game values | Plain string store, usable before startup; no weather side effects. Values survive shutdown until library unload. Size includes the terminating NUL. Missing values return size zero and empty output. Short buffers return required size with empty output. |
| Present callbacks | Registered callback runs outside the API lock after successful Present, including skipped presentation. Can re-enter the API. Nested Present does not dispatch it again. |
| Scene callbacks | Registerable; no scene events are emitted. Shutdown clears callbacks. |
| Mesh / material / texture | Shared validated resources for both backends. Nonzero hashes identify handles; creation copies input and replaces the current version atomically. Identical resubmissions retain the version. GPU allocation/upload occurs during Present. Queued draws retain resource versions. Destroy is idempotent for unknown nonnull handles. |
| Camera / DrawInstance | Matrix or parameterized world/sky/view-model cameras, triangle surfaces, CPU skinning and expanded instancing. Inputs are copied. Hidden/ignored/shadow-only instances are skipped. |
| Game UI | Register/update/free textures and submit a copied per-frame draw list. Flat commands use painter order; depth-enabled commands share a separate UI depth buffer. Texture id zero is white. |
| DrawScreenOverlay / SetScreenTint | Functional persistent overlay and linear-color tint. Null pixels with zero dimensions/opacity clear the overlay; nonpositive tint alpha disables tint. Game UI is composited afterward. |
| Lights | Distant-light creation/update copies direction and radiance; the last distant light drawn in a frame supplies tracing illumination. Destroy unregisters it. Raster does not evaluate lighting. Other light types retain tracked handles and no visual effect; AutoInstancePersistentLights remains a successful no-op. |
| Fog / camera medium / texture categories | Valid calls succeed without visual effects. Game-value weather keys remain plain stored strings. |
| VRAM maintenance requests | Successful no-ops. The renderer already reclaims retired resources after frame completion. |
| Picking / screenshot / VRAM statistics | Valid calls return NOT_INITIALIZED before startup and FEATURE_UNAVAILABLE afterward. Output statistics are zero. |
| Direct3D interop (Windows) | Every legacy interface slot is populated with a stub. Output objects, image/semaphore handles, dimensions and hashes are cleared. |
| Legacy exported ImGui/ImPlot widgets | Inert compatibility functions. Boolean/int results are zero; output coordinates are zero; input values are untouched. Draw callbacks are never invoked. No old ImGui ABI dependency. |

Startup, event pumping, window/input operations, Present and Shutdown belong to the
startup thread (the application main thread on macOS). Wrong-thread operations return
WRONG_THREAD. Call Shutdown and unload on this thread. API calls are serialized;
callers must finish concurrent calls and callbacks before unloading the library.
A Vulkan device/submission failure makes subsequent Present calls fail until Shutdown
and a new Startup. The runtime never waits on a fence left unsignaled by a failed submission.
Acquisition and frame-fence waits are bounded; shutdown and swapchain replacement must
still rely on the Vulkan driver's completion/device-loss reporting for submitted work.

New rendering calls require startup; malformed inputs return INVALID_ARGUMENTS and
unsupported formats/operations return FEATURE_UNAVAILABLE. Creation outputs are null
on failure. Rendering calls return GENERAL_FAILURE after a terminal GPU failure.
Resource calls may run on other threads; Vulkan work is deferred to the startup thread.
Present consumes pending geometry/UI before dispatching its callback, so re-entrant
submissions belong to the next frame. CPU copies and GPU objects needed by in-flight
frames remain alive through their fence. Materials retain their texture version;
recreate a material to use a replaced uploaded texture. Meshes resolve their material
handles at DrawInstance; already queued draws retain the resolved material version.

See [supported material/UI semantics and limits](raster-renderer-design.md).
The independent `remix_native_ext.h` adds backend capability/statistics exports and
optional stable instance identity/classification and camera temporal epoch extensions without changing the
0.1003 interface. See [path tracing](path-tracing.md) for their contracts.

On Windows the library is `remix.dll`. Startup with a null HWND creates a resizable
1280×720 SDL3 window. A supplied HWND is wrapped using SDL3 window properties; its
native ownership remains with the caller and Shutdown preserves it. Present may use
null or the startup window as `hwndOverride`; alternate windows and
`forceNoVkSwapchain` startup requests report FEATURE_UNAVAILABLE.

VSync uses FIFO when enabled. When disabled the shell selects Immediate, then Mailbox,
then FIFO. It is independent of all former RTX settings. Resize and VSync changes
recreate only presentation resources. Android background/resume checks surface identity
and recreates stale surfaces before acquiring another image.
