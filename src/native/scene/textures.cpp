#include "scene/images.h"
#include "scene/resources.h"
#include "scene/validation.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>

namespace remix {
  using namespace scene;
  using namespace scene::detail;

  namespace scene {
    uint64_t Texture::fingerprint() const {
      if (!contentHash) {
        uint64_t hash = 1469598103934665603ull;
        auto append = [&](const void *data, size_t size) { hash = hashBytes(data, size, hash); };
        append(&pixels.width, 4);
        append(&pixels.height, 4);
        append(&pixels.srgb, sizeof(bool));
        append(&pixels.twoChannel, sizeof(bool));
        for (const auto &mip : pixels.mips)
          append(mip.data(), mip.size());
        for (const auto &mip : pixels.hdrMips)
          append(mip.data(), mip.size() * sizeof(float));
        contentHash = hash ? hash : 1;
      }
      return contentHash;
    }
  } // namespace scene

  std::shared_ptr<Texture> NativeResources::texturePath(remixapi_Path path) {
    if (!m_white) {
      auto white = std::make_shared<Texture>();
      white->pixels = {1, 1, true, {{255, 255, 255, 255}}};
      m_white = std::move(white);
    }
    if (!path || !*path) {
      return m_white;
    }
    const std::filesystem::path name(path);
    const auto text = name.u8string();
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
      uint64_t hash = 0;
      if (text.size() > 18) {
        throw invalid;
      }
      for (size_t i = 2; i < text.size(); ++i) {
        const auto c = text[i];
        unsigned digit = c >= '0' && c <= '9'   ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : 16;
        if (digit > 15) {
          throw invalid;
        }
        hash = hash * 16 + digit;
      }
      auto it = m_textures.find(hash);
      if (it == m_textures.end()) {
        throw invalid;
      }
      return it->second;
    }
    const auto normalized = std::filesystem::absolute(name).lexically_normal();
    if (auto it = m_paths.find(normalized); it != m_paths.end()) {
      if (auto existing = it->second.lock()) {
        return existing;
      }
    }
    auto texture = std::make_shared<Texture>();
    texture->pixels = loadTexture(normalized);
    m_paths[normalized] = texture;
    return texture;
  }

  std::shared_ptr<Texture> NativeResources::dataTexturePath(remixapi_Path path) {
    if (!path || !*path)
      return {};
    auto source = texturePath(path);
    if (!source->pixels.srgb)
      return source;
    if (auto linear = source->linear.lock())
      return linear;
    auto linear = std::make_shared<Texture>();
    linear->pixels = source->pixels;
    linear->pixels.srgb = false; // Data maps store values, not sRGB colors.
    linear->encodedSource = source;
    source->linear = linear;
    return linear;
  }

  void NativeResources::createTexture(const remixapi_TextureInfo &info, remixapi_TextureHandle &handle) {
    if (!info.hash) {
      throw invalid;
    }
    auto texture = std::make_shared<Texture>();
    texture->pixels = decodeTexture(info);
    auto previous = m_textures.find(info.hash);
    if (previous == m_textures.end() || previous->second->pixels.width != texture->pixels.width ||
        previous->second->pixels.height != texture->pixels.height ||
        previous->second->pixels.srgb != texture->pixels.srgb ||
        previous->second->pixels.twoChannel != texture->pixels.twoChannel ||
        previous->second->pixels.mips != texture->pixels.mips ||
        previous->second->pixels.hdrMips != texture->pixels.hdrMips)
      m_textures[info.hash] = std::move(texture);
    handle = reinterpret_cast<remixapi_TextureHandle>(uintptr_t(info.hash));
  }

  void NativeResources::destroyTexture(remixapi_TextureHandle handle) { m_textures.erase(key(handle)); }
} // namespace remix
