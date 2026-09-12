#pragma once
#include <cstdint>

namespace remix {
  // CPU-writable GPU resources use the same bounded submission slots in every backend.
  inline constexpr uint32_t gpuFrameCount = 2;
} // namespace remix
