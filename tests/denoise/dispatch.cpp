#include <NRD.h>
#include "denoise/shaders/nrd_embedded.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace {
  void require(bool condition, const char *message) {
    if (!condition) {
      std::fprintf(stderr, "%s\n", message);
      std::exit(1);
    }
  }
  void ok(nrd::Result result) { require(result == nrd::Result::SUCCESS, "NRD rejected settings"); }
} // namespace

int main() {
  const auto &library = *nrd::GetLibraryDesc();
  require(library.versionMajor == 4 && library.versionMinor == 18 && library.versionBuild == 0,
          "NRD revision changed");
  require(library.normalEncoding == nrd::NormalEncoding::R10_G10_B10_A2_UNORM &&
              library.roughnessEncoding == nrd::RoughnessEncoding::LINEAR,
          "CPU and shader guide encodings differ");
  const auto &offset = library.spirvBindingOffsets;
  require(offset.samplerOffset == 0 && offset.constantBufferOffset == 2 &&
              offset.storageTextureAndBufferOffset == 3 && offset.textureOffset == 20,
          "SPIR-V binding contract changed");
  std::set<std::string> reached;
  for (auto kind : {nrd::Denoiser::REBLUR_DIFFUSE_SH, nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY, nrd::Denoiser::REBLUR_DIFFUSE, nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR}) {
    nrd::DenoiserDesc denoiser{0, kind};
    nrd::InstanceCreationDesc create{};
    create.denoisers = &denoiser;
    create.denoisersNum = 1;
    nrd::Instance *instance = nullptr;
    ok(nrd::CreateInstance(create, instance));
    const auto &desc = *nrd::GetInstanceDesc(*instance);
    require(desc.resourcesSpaceIndex == 0 && desc.constantBufferAndSamplersSpaceIndex == 1,
            "NRD descriptor spaces changed");
    for (unsigned frame = 0; frame < 12; ++frame) {
      nrd::CommonSettings common{};
      for (unsigned i = 0; i < 16; i += 5) {
        common.viewToClipMatrix[i] = common.viewToClipMatrixPrev[i] = 1;
        common.worldToViewMatrix[i] = common.worldToViewMatrixPrev[i] = 1;
      }
      common.resourceSize[0] = common.resourceSizePrev[0] = common.rectSize[0] = common.rectSizePrev[0] = 1920;
      common.resourceSize[1] = common.resourceSizePrev[1] = common.rectSize[1] = common.rectSizePrev[1] = 1080;
      common.motionVectorScale[0] = 1.f / 1920;
      common.motionVectorScale[1] = 1.f / 1080;
      common.isHistoryConfidenceAvailable = true;
      common.enableValidation = frame % 2;
      common.splitScreen = frame >= 8 ? (frame >= 10 ? 1.f : .5f) : 0.f;
      common.frameIndex = frame;
      common.accumulationMode =
          frame == 0 ? nrd::AccumulationMode::CLEAR_AND_RESTART : nrd::AccumulationMode::CONTINUE;
      common.timeDeltaBetweenFrames = frame % 2 ? 33.f : 8.f;
      nrd::ReblurSettings settings{};
      settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::OFF;
      if ((kind == nrd::Denoiser::REBLUR_DIFFUSE || kind == nrd::Denoiser::REBLUR_DIFFUSE_SPECULAR) && frame >= 2 && frame < 4) {
        settings.hitDistanceReconstructionMode = nrd::HitDistanceReconstructionMode::AREA_3X3;
        settings.diffusePrepassBlurRadius = 4.f;
      }
      settings.maxStabilizedFrameNum = frame < 4 ? 0 : nrd::REBLUR_MAX_HISTORY_FRAME_NUM;
      ok(nrd::SetCommonSettings(*instance, common));
      if (kind == nrd::Denoiser::SIGMA_SHADOW_TRANSLUCENCY) {
        nrd::SigmaSettings shadow{};
        shadow.lightDirection[1] = 1;
        shadow.maxStabilizedFrameNum = frame < 4 ? 0 : nrd::SIGMA_MAX_HISTORY_FRAME_NUM;
        ok(nrd::SetDenoiserSettings(*instance, 0, &shadow));
      } else
        ok(nrd::SetDenoiserSettings(*instance, 0, &settings));
      const nrd::Identifier identifier = 0;
      const nrd::DispatchDesc *dispatches = nullptr;
      uint32_t count = 0;
      ok(nrd::GetComputeDispatches(*instance, &identifier, 1, dispatches, count));
      require(count > 0 && count <= desc.descriptorPoolDesc.setsMaxNum, "Dispatch allocation bound changed");
      for (uint32_t i = 0; i < count; ++i) {
        const auto &dispatch = dispatches[i];
        const auto &pipeline = desc.pipelines[dispatch.pipelineIndex];
        bool embedded = false;
        for (const auto &shader : remix::denoise::shaders)
          embedded |= !std::strcmp(shader.identifier, pipeline.shaderIdentifier);
        require(embedded, pipeline.shaderIdentifier);
        require(!pipeline.computeShaderSPIRV.size && !pipeline.computeShaderDXIL.size &&
                    !pipeline.computeShaderDXBC.size,
                "An upstream embedded backend was enabled");
        require(dispatch.constantBufferDataSize <= desc.constantBufferMaxDataSize && dispatch.gridWidth &&
                    dispatch.gridHeight,
                "Dispatch constants/grid contract changed");
        reached.insert(pipeline.shaderIdentifier);
      }
    }
    nrd::DestroyInstance(*instance);
  }
  require(reached.size() == sizeof(remix::denoise::shaders) / sizeof(remix::denoise::shaders[0]),
          "A compiled permutation was not exercised");
  std::printf("All %zu pinned diffuse SH/radiance, SIGMA, clear and validation permutations exercised\n", reached.size());
}
