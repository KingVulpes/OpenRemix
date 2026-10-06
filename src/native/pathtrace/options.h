#pragma once
#include "upscale/options.h"
#include <cstdint>

namespace remix {
  struct TraceOptions {
    uint32_t samples = 1, bounces = 2;
    bool russianRoulette = false;
    uint32_t rouletteMinBounces = 1;
    bool accumulate = false, forceRebuild = false;
    bool denoiser = true, taa = true, validation = false;
    upscale::Mode upscaler = upscale::Mode::Auto;
    bool dlssRayReconstruction = true;
    uint32_t dlssQuality = 1;
    bool prism = true, gamma22 = true;
    bool autoExposure = true, bloom = true;
    float exposure = 1, autoExposureTarget = .2f, autoExposureSpeed = 2;
    float autoExposureCentre = .75f, autoExposureMinEv = -16, autoExposureMaxEv = 2;
    float bloomIntensity = .1f, bloomRadius = .85f;
    uint32_t fsrQuality = 1;
    float fsrSharpness = 0.f;
    bool sigma = true;
    bool blueNoiseShadows = true;
    bool blueNoiseLocalLights = true;
    bool localLights = true, localDenoiser = true;
    bool torchLights = true, emissiveLights = true;
    uint32_t emissiveCandidates = 4;
    // RIS candidates in sets of more than four sphere lights without emissive
    // surfaces (1: plain BVH sampling). Candidates compare unshadowed
    // contributions, cone shaping included, before the one shadow ray.
    uint32_t sphereCandidates = 1;
    bool adaptiveLocalSampling = true;
    bool stabilizeEmissiveLighting = false, emissiveSolidAngleSampling = false;
    bool freezeEmissiveRadiance = false; // diagnostic: casting only
    uint32_t separateLocalLighting = 0;  // combined, shared rays, per-family rays, combined separate pass
    uint32_t localSamples = 1;
    float localIntensity = 1, localRange = 16;
    float localDirectClamp = 0, indirectClamp = 0;
    bool localFireflyFilter = false;
    bool localGuiding = true;
    // Visibility-guiding cell size in meters (see sceneScale).
    float localGuideCellSize = 2;
    // Local-light (torch) REBLUR: anti-lag variance scale and sensitivity
    // (NRD ReblurAntilagSettings; lower values react sooner) and history cap.
    float localAntilagSigma = 2, localAntilagSensitivity = 3;
    uint32_t localHistoryFrames = 16;
    // Drop local-light history where moved lights or moved occluders (a closing
    // door; ReSTIR history samples re-tested) changed the light.
    bool localHistoryConfidence = true;
    // Trace an indirect continuation path from each transparent sheet (glass,
    // blended surfaces). Off: a non-PBR sheet takes the denoised indirect light
    // of the surface behind it (much cheaper; transparent layers cost about a
    // third). PBR sheets (water) always trace: their reflections need it.
    bool translucentIndirect = false;
    // Shadow, light-visibility and bounce rays ignore blended layers (glass,
    // sprites): no transmission through them and no candidate alpha tests.
    // Camera and transparency rays still see them.
    bool skipBlendedSecondaryRays = true;
    // Primary surface replacement: with raster primary visibility and a
    // denoiser, the surface a mirror (smooth metal or smooth black) reflects
    // replaces it as the pixel's primary surface, so it gets primary-quality
    // lighting and denoising at its mirrored position.
    bool primarySurfaceReplacement = true;
    // Without traced sheet indirect: PBR sheets (water) also sample local
    // lights at their own surface. Off: sun and sky only there (local light
    // still reaches them through their reflection paths).
    bool sheetLocalLights = false;
    // Exclude local lights outside the shading point's potentially visible
    // clusters (remixapi_SetVisibilityPartitionEXT), when a partition is set.
    // ReSTIR DI for primary local lighting (implies separateLocalLighting 3):
    // per-pixel reservoirs of light samples, resampled from the light tree,
    // reused across frames and neighbors, occluded samples discarded.
    bool restir = false;
    // Presampled light pools per world cell (ReGIR): bounce vertices, the
    // SHARC update and ReSTIR's initial candidates take a light from their
    // cell's pool instead of descending the light tree per sample.
    bool regir = true;
    float regirCellSize = 1; // meters (see sceneScale)
    // ORCA: a radiance cache rebuilt every frame from sparse full paths (one
    // per orcaTile x orcaTile pixels); other pixels read their first bounce
    // from it. No history: indirect light follows lighting changes at once.
    bool orca = false;
    uint32_t orcaTile = 4;
    float orcaCellSize = .0625f; // smallest voxel, meters; levels grow by 4x
    uint32_t orcaLevels = 6, orcaMinSamples = 4;
    // A voxel whose mean has a larger relative standard error than this is
    // passed over for the next larger one (then SHARC, then tracing on):
    // a few disagreeing samples flash as a patch. 0 accepts any voxel.
    float orcaMaxError = .5f;
    // Indirect-light denoiser history cap in frames (0 = NRD's default).
    uint32_t indirectHistoryFrames = 0;
    uint32_t restirCandidates = 8;     // initial light-tree candidates per pixel
    uint32_t restirSpatialSamples = 2; // neighbor reservoirs per pixel
    float restirSpatialRadius = 30;    // pixels
    // Temporal M cap in multiples of the current M. Longer histories correlate
    // the signal over time, which NRD cannot filter (frozen noise); dxvk-remix
    // uses 4.
    uint32_t restirTemporalHistory = 4;
    // RTXDI permutation sampling: the temporal lookup takes a per-frame
    // shuffled neighbor (2x2 block), decorrelating reused samples.
    bool restirPermutation = true;
    bool restirInitialVisibility = true; // shadow-test the initial winner before reuse
    bool lightVisibilityCulling = false; // opt-in: ~3 ms on L4D2 c1m1, no gain where the PVS is broad
    // Meters per world unit. Scales meter-based defaults: denoiser hit
    // distances and guiding cells (Source engine inches: 0.0254).
    float sceneScale = 1;
    uint32_t shadowDebug = 0;
    uint32_t debugView = 0;        // display diagnostics; never selects an integrator
    float sunAngularDiameter = -1; // -1 inherits the distant light's diameter.
    bool rasterPrimary = true;
    bool waterIndirectReuse = true;
    bool translucencyDenoiser = true, translucencyReuse = false;
    bool pbrLocalReuse = false; // Experimental previous-frame secondary local lighting.
    bool sky = true;
    bool sharc = true, sharcDebug = false;
    uint32_t sharcCapacityLog2 = 22, sharcDownscale = 5, sharcSceneScale = 50;
    uint32_t sharcAccumulationFrames = 32, sharcStaleFrames = 64;
    uint64_t sharcReset = 0; // Runtime action sequence; excluded from profiles.
    bool overrideLight = false;
    float lightDirection[3] = {-0.4f, -0.8f, -0.3f};
    float lightColor[3] = {1, 1, 1};
    float lightIntensity = 3.14159265f;
    // GPU optimizations, each off by default and output-identical when on
    // (docs in ui/rendering.cpp). All apply at runtime without a restart.
    // One-sided surfaces become hardware-opaque in their BLAS and one-sided
    // instances reject back faces in traversal instead of in the shader.
    bool hardwareFaceCulling = false;
    // One TLAS over the static and dynamic instances: one traversal per ray.
    bool combinedTlas = false;
    // Boolean shadow and local-light rays stop at their first blocker.
    bool shadowEarlyOut = false;
    bool operator==(const TraceOptions &) const = default;
  };
  // Options as the tracer applies them: ReSTIR DI runs in the combined
  // separate local-lighting pass.
  inline TraceOptions normalized(TraceOptions options) {
    if (options.restir)
      options.separateLocalLighting = 3;
    return options;
  }
} // namespace remix
