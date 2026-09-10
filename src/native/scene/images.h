#pragma once
#include "scene/types.h"
#include <filesystem>

namespace remix::scene {
  Pixels decodeTexture(const remixapi_TextureInfo &info);
  Pixels loadTexture(const std::filesystem::path &path);
} // namespace remix::scene
