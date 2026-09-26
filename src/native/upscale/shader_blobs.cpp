#include "shaders/fsr31.h"
#include <FidelityFX/host/ffx_interface.h>
#include <cstring>
extern "C" FfxErrorCode ffxGetPermutationBlobByIndex(FfxEffect effect,
                                                     FfxPass pass, FfxBindStage,
                                                     uint32_t flags,
                                                     FfxShaderBlob *out) {
  // Our context uses HDR, inverted depth and unjittered render-resolution
  // motion. FP32 shaders also work on devices whose logical device did not
  // enable FP16.
  if (!out || effect != FFX_EFFECT_FSR3UPSCALER || pass > 10 ||
      (flags & (8 | 128)) || (flags & (6 | 16)) != (6 | 16) || (flags & ~255u))
    return FFX_ERROR_INVALID_ARGUMENT;
  std::memcpy(out, &remix::upscale::blobs[pass + ((flags & 1) ? 11 : 0)],
              sizeof(*out));
  return FFX_OK;
}
extern "C" FfxErrorCode ffxIsWave64(FfxEffect effect, uint32_t flags,
                                    bool &wave64) {
  if (effect != FFX_EFFECT_FSR3UPSCALER)
    return FFX_ERROR_INVALID_ARGUMENT;
  wave64 = (flags & 64) != 0;
  return FFX_OK;
}
// Upscaling does not create or replace the application's swapchain.
extern "C" FfxErrorCode
ffxSetFrameGenerationConfigToSwapchainVK(const FfxFrameGenerationConfig *) {
  return FFX_ERROR_INVALID_ARGUMENT;
}
