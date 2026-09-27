#pragma once
#include "scene/types.h"
#include <filesystem>

namespace remix::scene {
  Pixels decodeTexture(const remixapi_TextureInfo &info);
  Pixels loadTexture(const std::filesystem::path &path);
  // BC1/BC3/BC4/BC5/BC6H/BC7 formats that compressTexture accepts.
  bool isBlockCompressed(remixapi_Format format);
  // Validates BC data like decodeTexture and keeps the blocks for GPU upload.
  // Only mip-0 alpha coverage bits and a <=64x64 preview are decoded.
  Pixels compressTexture(const remixapi_TextureInfo &info);
  // decodeTexture's result for a compressed texture whose upload payload is
  // still present (used when the device cannot sample BC formats).
  Pixels decodeCompressed(const Pixels &pixels);
} // namespace remix::scene
