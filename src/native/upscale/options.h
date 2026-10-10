#pragma once
#include <cstdint>

namespace remix::upscale {
  enum class Mode : uint32_t { Off, Auto, Fsr31, Dlss };
  // "rtx.native.frameGenerationBackend". Auto: DLSS Frame Generation where
  // NGX reports it available, FSR 3.1 otherwise.
  enum class FrameGenerationMode : uint32_t { Auto, Dlss, Fsr31 };
}
