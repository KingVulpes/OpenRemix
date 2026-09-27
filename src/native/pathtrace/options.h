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
    bool adaptiveLocalSampling = true;
    bool stabilizeEmissiveLighting = false, emissiveSolidAngleSampling = false;
    bool freezeEmissiveRadiance = false; // diagnostic: casting only
    uint32_t separateLocalLighting = 0;  // combined, shared rays, per-family rays, combined separate pass
    uint32_t localSamples = 1;
    float localIntensity = 1, localRange = 16;
    float localDirectClamp = 0, indirectClamp = 0;
    bool localFireflyFilter = false;
    bool localGuiding = true;
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
    bool operator==(const TraceOptions &) const = default;
  };
} // namespace remix
