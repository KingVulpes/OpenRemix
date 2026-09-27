/*
 * Fork-compatible extensions (from Kim2091/dxvk-remix rebased-9-19-opt) needed by
 * the Fallout 4 plugin. Local OpenRemix branch only. Declarations only: an export
 * listed here is present in remix.dll only once the runtime implements it, so
 * hosts must keep probing with GetProcAddress. Included from remix_c.h.
 */

#ifndef REMIX_FO4_EXT_H_
#define REMIX_FO4_EXT_H_

#ifdef __cplusplus
extern "C" {
#endif

  // Valid only if remixapi_MaterialInfo contains remixapi_MaterialInfoOpaqueEXT
  // in its pNext chain. Describes a specular/gloss source map bound as the
  // roughness texture, for hosts whose content uses that workflow.
  // Check remixapi_GetSpecGlossMaterialVersion() before chaining it: a runtime
  // that predates it ignores the struct and would read the map as roughness.
  typedef struct remixapi_MaterialInfoOpaqueSpecGlossEXT {
    remixapi_StructType sType;
    void*               pNext;
    // REMIXAPI_TEXTURE_ENCODING_* of the roughness texture. Applies when the
    // texture itself carries no encoding (uploaded with remixapi_CreateTexture);
    // an encoding given at import wins. SMOOTHNESS_IN_* makes roughnessConstant
    // the lower bound of the decoded roughness, as for imported textures.
    uint32_t            roughnessEncoding;
    // Per-pixel metalness from the specular map. Requires a SMOOTHNESS_IN_GREEN
    // roughness texture, whose red channel then holds the (gamma-encoded)
    // specular level. metallic = metalStrength * ramp(spec * max(specularTint))
    // between specularMetalLow and specularMetalHigh, attenuated where the
    // albedo luminance (gamma) approaches metalAlbedoLuminanceMax (>= 1: off).
    // metalStrength 0 disables it; the material's metallicConstant still
    // applies where the map is not sampled.
    float               metalStrength;
    float               specularMetalLow;
    float               specularMetalHigh;
    float               metalAlbedoLuminanceMax;
    // Specular colour (gamma, 0..1). Non-zero: metal texels take spec * tint as
    // their albedo (their F0). Zero: albedo is kept and the ramp uses spec alone.
    remixapi_Float3D    specularTint;
  } remixapi_MaterialInfoOpaqueSpecGlossEXT;
  // Returns the supported revision of remixapi_MaterialInfoOpaqueSpecGlossEXT
  // (currently 1). Optional named export; absent on older runtimes.
  typedef uint32_t(REMIXAPI_PTR* PFN_remixapi_GetSpecGlossMaterialVersion)(void);
  REMIXAPI uint32_t REMIXAPI_CALL remixapi_GetSpecGlossMaterialVersion(void);

  // Valid only if remixapi_MaterialInfo contains remixapi_MaterialInfoOpaqueEXT
  // in its pNext chain. Multiplies the RGB of the sampled albedo texture by
  // albedoTint, so one untinted texture can be shared by differently tinted
  // materials (skin/hair tints). The multiply happens on the texture's encoded
  // (gamma) values before the shader linearizes them, the same as multiplying
  // 8-bit texels on the CPU; for an sRGB-format texture (sampled linear) the
  // tint is linearized first. Alpha, albedoConstant (used when there is no
  // texture) and the other maps are untouched. (1,1,1) is a no-op. Components
  // are clamped to [0, 16].
  // Check remixapi_GetMaterialAlbedoTintVersion() before relying on it: a
  // runtime that predates it ignores the struct and renders the texture
  // untinted.
  typedef struct remixapi_MaterialInfoOpaqueAlbedoTintEXT {
    remixapi_StructType sType;
    void*               pNext;
    remixapi_Float3D    albedoTint;
  } remixapi_MaterialInfoOpaqueAlbedoTintEXT;
  // Returns the supported revision of remixapi_MaterialInfoOpaqueAlbedoTintEXT
  // (currently 1). Optional named export; absent on older runtimes.
  typedef uint32_t(REMIXAPI_PTR* PFN_remixapi_GetMaterialAlbedoTintVersion)(void);
  REMIXAPI uint32_t REMIXAPI_CALL remixapi_GetMaterialAlbedoTintVersion(void);

  // Link into remixapi_MeshInfo::pNext to REPLACE the geometry of a mesh that
  // is already registered under the same hash, instead of the default behavior
  // of ignoring the repeated registration.
  //
  // Intended for CPU-deformed geometry whose topology is fixed but whose vertex
  // positions are rewritten by the host every few frames (e.g. Fallout 4 FaceGen
  // morphs for lip sync, blinks, and expressions). Re-using the hash is what
  // makes this work: the mesh handle IS the hash, and the handle feeds both
  // ExternalDrawState::computeExternalDrawIdentityHash and the spatial-map hash,
  // so minting a fresh hash per update would hand the scene manager a brand-new
  // object every time -- a new RtInstance with no previous-frame correspondence,
  // i.e. no motion vectors and no denoiser history (ghosting).
  //
  // On refresh, the runtime carries the previous registration's topology and
  // layout hashes over to the new surfaces and leaves only VertexPosition fresh.
  // DrawCallCache therefore keeps the same BlasEntry (it buckets on
  // TopologicalHash) and processGeometryInfo resolves to kUpdateBVH, which
  // ping-pongs the history buffers and populates previousPositionBuffer --
  // giving the morph correct motion vectors rather than merely stable ones.
  //
  // The refresh is ignored (falls back to leaving the existing mesh alone) if no
  // mesh is registered under the hash, or if the surface count differs from the
  // registered one -- a changed surface count is a different mesh, not a morph.
  typedef struct remixapi_MeshInfoRefreshGeometryEXT {
    remixapi_StructType sType;
    void*               pNext;
  } remixapi_MeshInfoRefreshGeometryEXT;

#if defined(_WIN32)
  // Optional named exports; the versioned Interface table is unchanged.
  // Prepare on the source D3D11 immediate-context thread, outside deferred
  // recording. Only IMMUTABLE or DEFAULT, shader-only, single-layer 2D images
  // are accepted; later GPU writes to a DEFAULT image reach Remix in order.
  // dxgiFormat selects a compatible typed view. No pixel copies or GPU idle wait.
  // The token retains the image and its device; release it after Import returns.
  // Import queues a read barrier on Remix's shared graphics queue. DestroyTexture
  // retires the registered handle using the existing texture-manager lifetime.
  typedef void* remixapi_D3D11TextureToken;
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_PrepareD3D11Texture)(
    void* texture2D, uint32_t dxgiFormat, remixapi_D3D11TextureToken* out_token);
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_ImportD3D11Texture)(
    remixapi_D3D11TextureToken token, uint64_t hash, remixapi_TextureHandle* out_handle);
  typedef void(REMIXAPI_PTR* PFN_remixapi_ReleaseD3D11Texture)(remixapi_D3D11TextureToken token);

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PrepareD3D11Texture(
    void* texture2D, uint32_t dxgiFormat, remixapi_D3D11TextureToken* out_token);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ImportD3D11Texture(
    remixapi_D3D11TextureToken token, uint64_t hash, remixapi_TextureHandle* out_handle);
  REMIXAPI void REMIXAPI_CALL remixapi_ReleaseD3D11Texture(remixapi_D3D11TextureToken token);

  // Import that also names how the image stores its data, so a material using
  // it decodes the host's own encoding instead of the host converting pixels.
  // An encoding only affects the material slot it is meaningful for.
  #define REMIXAPI_TEXTURE_ENCODING_NONE                0u
  #define REMIXAPI_TEXTURE_ENCODING_NORMAL_TANGENT_XY   1u  // normal: unsigned X/Y, Z rebuilt
  #define REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_RED   2u  // roughness = 1 - red
  #define REMIXAPI_TEXTURE_ENCODING_SMOOTHNESS_IN_GREEN 3u  // roughness = 1 - green
  // With a SMOOTHNESS_IN_* roughness texture, the opaque material's
  // roughnessConstant is the lower bound applied to the decoded roughness.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_ImportD3D11TextureEx)(
    remixapi_D3D11TextureToken token, uint64_t hash, uint32_t encoding, remixapi_TextureHandle* out_handle);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_ImportD3D11TextureEx(
    remixapi_D3D11TextureToken token, uint64_t hash, uint32_t encoding, remixapi_TextureHandle* out_handle);

  // Prepare that also caps the imported mip chain: mips larger than
  // maxDimension texels on their longest side are left out, so Remix starts
  // at the first mip that fits (Remix's copy then holds only those mips).
  // 0 imports the full chain, as PrepareD3D11Texture does. A single-mip image
  // is never cut. The runtime option rtx.hostImport.maxCopiedTextureDimension,
  // when set, applies on top as a second cap.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_PrepareD3D11TextureEx)(
    void* texture2D, uint32_t dxgiFormat, uint32_t maxDimension, remixapi_D3D11TextureToken* out_token);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_PrepareD3D11TextureEx(
    void* texture2D, uint32_t dxgiFormat, uint32_t maxDimension, remixapi_D3D11TextureToken* out_token);

  // Host settings: a host (game plugin) exposes its own options as live
  // controls in the developer menu's "Host Settings" tab. Optional named
  // exports; the versioned Interface table is unchanged. Probe with
  // GetProcAddress(dll, "remixapi_RegisterHostSetting").
  //
  // Every value is a double: BOOL 0/1, INT/ENUM an integral value, FLOAT as
  // is, ACTION a click counter the menu increments (a button). The runtime
  // stores the value; the menu edits it. The host reads
  // remixapi_GetHostSettingsGeneration() once per frame (an atomic load) and
  // only re-reads values when it changed. All functions are thread-safe and
  // may be called from any thread.
  typedef enum remixapi_HostSettingType {
    REMIXAPI_HOST_SETTING_TYPE_BOOL   = 0,
    REMIXAPI_HOST_SETTING_TYPE_INT    = 1,
    REMIXAPI_HOST_SETTING_TYPE_FLOAT  = 2,
    REMIXAPI_HOST_SETTING_TYPE_ENUM   = 3,  // value indexes enumLabels
    REMIXAPI_HOST_SETTING_TYPE_ACTION = 4,  // button; value counts clicks
  } remixapi_HostSettingType;

  // Shown as "(restart)"; highlighted once the value differs from the
  // value registered at startup.
  #define REMIXAPI_HOST_SETTING_FLAG_REQUIRES_RESTART 0x1u
  // Drawn disabled; the host may still update it with SetHostSettingValue.
  #define REMIXAPI_HOST_SETTING_FLAG_READ_ONLY        0x2u

  typedef struct remixapi_HostSettingInfo {
    uint32_t                 structSize;    // sizeof(remixapi_HostSettingInfo)
    const char*              key;           // stable unique id (required)
    const char*              label;         // NULL = key
    const char*              section;       // group header; NULL = "General"
    const char*              tooltip;       // optional
    remixapi_HostSettingType type;
    uint32_t                 flags;         // REMIXAPI_HOST_SETTING_FLAG_*
    double                   minValue;      // INT/FLOAT range; min >= max = unbounded
    double                   maxValue;
    double                   step;          // drag speed; 0 = derived from the range
    double                   defaultValue;  // target of the reset button
    double                   currentValue;  // the host's live value
    const char* const*       enumLabels;    // ENUM: labels of values 0..enumCount-1
    uint32_t                 enumCount;
  } remixapi_HostSettingInfo;

  typedef uint32_t remixapi_HostSettingHandle;  // 0 = invalid

  // Strings are copied. Registering an existing key replaces its metadata and
  // value and returns the same handle.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_RegisterHostSetting)(
    const remixapi_HostSettingInfo* info, remixapi_HostSettingHandle* out_handle);
  typedef uint64_t(REMIXAPI_PTR* PFN_remixapi_GetHostSettingsGeneration)(void);
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_GetHostSettingValue)(
    remixapi_HostSettingHandle handle, double* out_value);
  // Host-side write (e.g. to reflect a clamp); does not bump the generation.
  typedef remixapi_ErrorCode(REMIXAPI_PTR* PFN_remixapi_SetHostSettingValue)(
    remixapi_HostSettingHandle handle, double value);
  // Drop every registered setting (host shutdown); invalidates all handles.
  typedef void(REMIXAPI_PTR* PFN_remixapi_ClearHostSettings)(void);

  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_RegisterHostSetting(
    const remixapi_HostSettingInfo* info, remixapi_HostSettingHandle* out_handle);
  REMIXAPI uint64_t REMIXAPI_CALL remixapi_GetHostSettingsGeneration(void);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_GetHostSettingValue(
    remixapi_HostSettingHandle handle, double* out_value);
  REMIXAPI remixapi_ErrorCode REMIXAPI_CALL remixapi_SetHostSettingValue(
    remixapi_HostSettingHandle handle, double value);
  REMIXAPI void REMIXAPI_CALL remixapi_ClearHostSettings(void);
#endif // _WIN32

#ifdef __cplusplus
}
#endif

// Native D3D11 host extensions. Resolve by name from the unified runtime.
// Register once on the game render thread; Shutdown releases only our reference.
// Render queues composition into a same-device single-sample render target.
// The selected swapchain's next Present ends the frame. render=false keeps raster output.
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_RegisterD3D11Device)(void* device);
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_RenderD3D11)(void* destination, remixapi_Bool render);

// Retained instances. Register a draw once and replay the whole table with
// one DrawRetainedInstances call per frame, instead of one DrawInstance per
// placement per frame. Changes are reported in a single batched call.
// Handles are nonzero. Destroy a retained instance before its mesh.
#define REMIXAPI_RETAINED_UPDATE_TRANSFORM  0x1u
#define REMIXAPI_RETAINED_UPDATE_VISIBILITY 0x2u
typedef struct remixapi_RetainedInstanceUpdate {
  uint64_t           handle;
  remixapi_Transform transform;   // absolute; valid with UPDATE_TRANSFORM
  uint32_t           flags;       // REMIXAPI_RETAINED_UPDATE_*
  remixapi_Bool      visible;     // valid with UPDATE_VISIBILITY
} remixapi_RetainedInstanceUpdate;
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_CreateRetainedInstance)(
  const remixapi_InstanceInfo* info, uint64_t* out_handle);
// Replace a registered draw's whole description (mesh, flags, instancing,
// bones, blend state). Keeps its visibility.
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_UpdateRetainedInstance)(
  uint64_t handle, const remixapi_InstanceInfo* info);
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_UpdateRetainedInstances)(
  const remixapi_RetainedInstanceUpdate* updates, uint32_t count);
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_DestroyRetainedInstances)(
  const uint64_t* handles, uint32_t count);
// sceneOrigin: 3 floats subtracted from every retained transform at replay.
// Null = no rebase. The host must rebase camera and lights to match.
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_DrawRetainedInstances)(
  const float* sceneOrigin);

// GPU screen overlay for a native D3D11 host: pass the host's UI render
// targets instead of CPU pixels (DrawScreenOverlay). Call on the thread that
// owns the registered immediate context. Each call records a GPU copy of every
// layer's mip 0 into a Remix-owned image on the host's command stream, after
// every draw issued before the call; the image is recreated only when the
// layer's size or format changes. The layer set replaces the previous one and
// keeps compositing every frame, over any DrawScreenOverlay image, until the
// next call; count 0 clears it. Layers composite bottom-first.
// Returns GENERAL_FAILURE while the runtime option rtx.enableGpuScreenOverlay
// is off (use DrawScreenOverlay instead), INVALID_ARGUMENTS for a texture it
// cannot copy (other device, multisampled, array, staging).
#define REMIXAPI_SCREEN_OVERLAY_LAYER_PREMULTIPLIED 0x1u  // RGB already multiplied by alpha
#define REMIXAPI_SCREEN_OVERLAY_MAX_LAYERS          8u
typedef struct remixapi_ScreenOverlayLayerD3D11 {
  void*    texture2D;  // ID3D11Texture2D* of the registered device
  uint32_t flags;      // REMIXAPI_SCREEN_OVERLAY_LAYER_*
  float    opacity;    // 0..1
  float    rect[4];    // destination x, y, width, height as output fractions; zero width or height = full output
} remixapi_ScreenOverlayLayerD3D11;
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_DrawScreenOverlayD3D11)(
  const remixapi_ScreenOverlayLayerD3D11* layers, uint32_t count);
// DrawScreenOverlayD3D11 with a per-layer alpha cutoff: where a layer's sampled
// alpha is at or below alphaCutoffs[i] (0..1) the layer is dropped and the output
// keeps its colour, as if the host had zeroed those pixels. 0 = no cutoff.
// alphaCutoffs may be null (same as DrawScreenOverlayD3D11), otherwise it holds
// count entries. Optional named export ("remixapi_DrawScreenOverlayD3D11Ex");
// its presence is the capability check.
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_DrawScreenOverlayD3D11Ex)(
  const remixapi_ScreenOverlayLayerD3D11* layers, uint32_t count, const float* alphaCutoffs);

#if defined(_WIN32)
// Replace the image behind a texture imported with ImportD3D11TextureEx
// (or ImportD3D11Texture) with a new D3D11 texture, keeping its handle, hash
// and texture-table slot: materials, meshes and instances that use the handle
// need no change and pick up the new image on the next frame. For streamed
// mip upgrades and downgrades of the same logical texture. token comes from
// PrepareD3D11Texture(Ex) exactly as for an import (same share / copy-on-write
// / copy and mip-cap rules); release it after the call returns. The old image
// is freed once no in-flight frame can read it (API materials created before
// the replace keep a reference to it until they are destroyed). If the handle
// was destroyed before the replace reaches the render thread, the replace is
// dropped. Returns GENERAL_FAILURE while the runtime option
// rtx.hostImport.enableInPlaceTextureReplace is off. Resolve by name
// ("remixapi_ReplaceD3D11TextureEx").
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_ReplaceD3D11TextureEx)(
  remixapi_D3D11TextureToken token, remixapi_TextureHandle handle, uint32_t encoding);
#endif // _WIN32

// Per-instance material override. Chain on remixapi_InstanceInfo (DrawInstance,
// CreateRetainedInstance, UpdateRetainedInstance). Every surface of the mesh is
// shaded with `material` instead of the material the mesh was created with, so
// a host can create one mesh per unique geometry and draw it with different
// materials. A null handle, or a handle that is not (or no longer) a live
// material, keeps the mesh's own material.
// With rtx.hostImport.shareGeometryAcrossMaterials on (default), instances of
// one mesh that carry this struct share one geometry copy and one BLAS across
// materials. Materials that alpha-test or blend (in the material or through
// remixapi_InstanceInfoBlendEXT) keep a geometry copy per material, so opacity
// micromaps stay per opacity texture. The option only changes sharing; the
// override is always applied.
// Check remixapi_GetInstanceMaterialOverrideVersion() (resolve by name) before
// relying on it: a runtime that predates it ignores the struct and shades with
// the mesh's own material.
typedef struct remixapi_InstanceInfoMaterialOverrideEXT {
  remixapi_StructType     sType;
  void*                   pNext;
  remixapi_MaterialHandle material;
} remixapi_InstanceInfoMaterialOverrideEXT;
// Returns the supported revision of remixapi_InstanceInfoMaterialOverrideEXT
// (currently 1). Optional named export ("remixapi_GetInstanceMaterialOverrideVersion").
typedef uint32_t (REMIXAPI_CALL* PFN_remixapi_GetInstanceMaterialOverrideVersion)(void);

// Device-local memory pressure as seen by the runtime's VRAM budget
// (rtx.vramBudget). usageBytes / budgetBytes are the driver's figures for the
// whole process (VK_EXT_memory_budget), so the host's own D3D11 resources on the
// shared device are included. The runtime reacts first (frees idle BLAS and
// scratch, shrinks caches, lowers the resolution of new texture copies); when
// requestHostRelease is set it has run out of cheap levers and asks the host to
// release content it can rebuild later, far content first, about
// bytesToRelease (what would bring usage back below the level-1 release point).
// level: 0 none, 1 elevated, 2 high, 3 critical. All zero while the option is
// off or VK_EXT_memory_budget is unavailable. Values are one frame old at most.
// Set structSize = sizeof(remixapi_MemoryPressureInfo) before the call. Any
// thread, no locks. Optional named export ("remixapi_GetMemoryPressure").
#define REMIXAPI_MEMORY_PRESSURE_NONE     0u
#define REMIXAPI_MEMORY_PRESSURE_ELEVATED 1u
#define REMIXAPI_MEMORY_PRESSURE_HIGH     2u
#define REMIXAPI_MEMORY_PRESSURE_CRITICAL 3u
typedef struct remixapi_MemoryPressureInfo {
  uint32_t structSize;          // in: sizeof(remixapi_MemoryPressureInfo)
  uint32_t level;               // REMIXAPI_MEMORY_PRESSURE_*
  uint32_t requestHostRelease;  // 1 = please release far content now
  uint32_t frame;               // runtime frame of the sample
  uint64_t usageBytes;          // device-local usage, whole process
  uint64_t budgetBytes;         // device-local budget
  uint64_t bytesToRelease;      // usage above the level-1 release point
} remixapi_MemoryPressureInfo;
typedef remixapi_ErrorCode (REMIXAPI_CALL* PFN_remixapi_GetMemoryPressure)(
  remixapi_MemoryPressureInfo* out_info);

#endif // REMIX_FO4_EXT_H_
