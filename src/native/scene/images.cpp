#include "scene/images.h"
#include <remix/remix_native_ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 16384
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <image/stb_image.h>
#define BCDEC_STATIC
#define BCDEC_IMPLEMENTATION
#include <image/bcdec.h>

namespace remix::scene {
  namespace {
    constexpr auto invalid = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
    constexpr auto unavailable = REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
    uint32_t word(const std::vector<uint8_t> &data, size_t offset) {
      if (offset + 4 > data.size()) {
        throw invalid;
      }
      return uint32_t(data[offset]) | uint32_t(data[offset + 1]) << 8 | uint32_t(data[offset + 2]) << 16 |
             uint32_t(data[offset + 3]) << 24;
    }
    Pixels decodeFloat(uint32_t width, uint32_t height, uint32_t mips, unsigned channels, bool half,
                       const uint8_t *data, size_t size) {
      if (!width || !height || width > 16384 || height > 16384 || !data || channels < 1 || channels > 4)
        throw invalid;
      unsigned levels = 0;
      for (auto n = std::max(width, height); n; n >>= 1)
        ++levels;
      if (mips > levels)
        throw invalid;
      Pixels out{width, height, false, {}};
      size_t offset = 0, total = 0;
      for (unsigned mip = 0; mip < std::max(1u, mips); ++mip) {
        size_t count = size_t(width) * height, bytes = count * channels * (half ? 2 : 4);
        total += count * 20;
        if (bytes > size - offset || total > 512u * 1024 * 1024)
          throw invalid;
        auto &floats = out.hdrMips.emplace_back(count * 4);
        auto &pixels = out.mips.emplace_back(count * 4);
        for (size_t i = 0; i < count; ++i)
          for (unsigned c = 0; c < 4; ++c) {
            float value = c == 3 ? 1 : 0;
            if (c < channels || (channels == 1 && c < 3)) {
              const size_t source = offset + (i * channels + (channels == 1 ? 0 : c)) * (half ? 2 : 4);
              if (half) {
                const unsigned bits = unsigned(data[source]) | (unsigned(data[source + 1]) << 8),
                               exponent = (bits >> 10) & 31, fraction = bits & 1023;
                value = exponent == 31 ? 0
                                       : std::ldexp(float(fraction + (exponent ? 1024 : 0)),
                                                    (exponent ? int(exponent) - 15 : -14) - 10);
                if (bits & 0x8000)
                  value = -value;
              } else {
                std::memcpy(&value, data + source, 4);
                if (!std::isfinite(value))
                  value = 0;
              }
            }
            floats[i * 4 + c] = value;
            pixels[i * 4 + c] = uint8_t(std::clamp(value, 0.f, 1.f) * 255);
          }
        offset += bytes;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
      }
      return out;
    }
    // defer: validate, then keep BC1-BC5/BC7 blocks for direct upload and lazy
    // CPU decoding. BC6H always decodes, since HDR consumers sanitize values.
    Pixels decode(uint32_t width, uint32_t height, uint32_t mips, bool srgb, uint32_t block, bool bgra,
                  const uint8_t *data, size_t size, bool defer = false, bool opaqueAlpha = false) {
      if (!width || !height || width > 16384 || height > 16384 || !data) {
        throw invalid;
      }
      uint32_t levels = 0;
      for (auto dimension = std::max(width, height); dimension; dimension >>= 1) {
        ++levels;
      }
      mips = std::max(1u, mips);
      if (mips > levels) {
        throw invalid;
      }
      Pixels out{width, height, srgb, {}};
      out.twoChannel = block == 5;
      size_t offset = 0, total = 0;
      if (defer && block && block != 6 && block != 16) {
        auto blocks = std::make_shared<BlockData>();
        blocks->block = block;
        blocks->opaqueAlpha = opaqueAlpha;
        for (uint32_t mip = 0; mip < mips; ++mip) {
          const size_t bytes = size_t((width + 3) / 4) * ((height + 3) / 4) * blocks->blockBytes();
          total += size_t(width) * height * 4;
          if (offset > size || bytes > size - offset || total > 512u * 1024 * 1024)
            throw invalid;
          blocks->offsets.push_back(offset);
          offset += bytes;
          width = std::max(1u, width / 2);
          height = std::max(1u, height / 2);
        }
        blocks->offsets.push_back(offset);
        blocks->data.assign(data, data + offset);
        out.blocks = std::move(blocks);
        return out;
      }
      for (uint32_t mip = 0; mip < mips; ++mip) {
        const size_t bytes =
            block ? size_t((width + 3) / 4) * ((height + 3) / 4) * ((block == 1 || block == 4) ? 8 : 16)
                  : size_t(width) * height * 4;
        const size_t decodedSize = size_t(width) * height * 4;
        total += decodedSize * ((block == 6 || block == 16) ? 5 : 1);
        if (offset > size || bytes > size - offset || total > 512u * 1024 * 1024) {
          throw invalid;
        }
        auto &pixels = out.mips.emplace_back(decodedSize);
        if (block == 6 || block == 16)
          out.hdrMips.emplace_back(decodedSize);
        if (!block) {
          std::memcpy(pixels.data(), data + offset, bytes);
          if (bgra) {
            for (size_t i = 0; i < pixels.size(); i += 4) {
              std::swap(pixels[i], pixels[i + 2]);
            }
          }
        } else {
          size_t cursor = offset;
          for (uint32_t y = 0; y < height; y += 4) {
            for (uint32_t x = 0; x < width; x += 4) {
              alignas(8) uint8_t source[16] = {}, result[64] = {};
              std::memcpy(source, data + cursor, (block == 1 || block == 4) ? 8 : 16);
              if (block == 1) {
                bcdec_bc1(source, result, 16);
              } else if (block == 2) {
                bcdec_bc2(source, result, 16);
              } else if (block == 3) {
                bcdec_bc3(source, result, 16);
              } else if (block == 4) {
                uint8_t scalar[16];
                bcdec_bc4(source, scalar, 4);
                for (int i = 0; i < 16; ++i) {
                  result[i * 4] = result[i * 4 + 1] = result[i * 4 + 2] = scalar[i];
                  result[i * 4 + 3] = 255;
                }
              } else if (block == 6 || block == 16) {
                float rgb[48];
                bcdec_bc6h_float(source, rgb, 12, block == 16);
                for (uint32_t row = 0; row < std::min(4u, height - y); ++row)
                  for (uint32_t col = 0; col < std::min(4u, width - x); ++col) {
                    const auto dst = (size_t(y + row) * width + x + col) * 4;
                    for (unsigned c = 0; c < 3; ++c) {
                      const float value = rgb[(row * 4 + col) * 3 + c];
                      out.hdrMips.back()[dst + c] = std::isfinite(value) ? value : 0;
                      result[(row * 4 + col) * 4 + c] =
                          uint8_t(std::clamp(out.hdrMips.back()[dst + c], 0.f, 1.f) * 255);
                    }
                    out.hdrMips.back()[dst + 3] = 1;
                    result[(row * 4 + col) * 4 + 3] = 255;
                  }
              } else if (block == 5) {
                uint8_t rg[32];
                bcdec_bc5(source, rg, 8);
                for (int i = 0; i < 16; ++i) {
                  result[i * 4] = rg[i * 2];
                  result[i * 4 + 1] = rg[i * 2 + 1];
                  result[i * 4 + 3] = 255;
                }
              } else if (block == 7) {
                bcdec_bc7(source, result, 16);
              } else {
                throw unavailable;
              }
              for (uint32_t row = 0; row < std::min(4u, height - y); ++row) {
                std::memcpy(pixels.data() + (size_t(y + row) * width + x) * 4, result + row * 16,
                            std::min(4u, width - x) * 4);
              }
              cursor += (block == 1 || block == 4) ? 8 : 16;
            }
          }
        }
        offset += bytes;
        width = std::max(1u, width / 2);
        height = std::max(1u, height / 2);
      }
      return out;
    }
  } // namespace

  const std::vector<std::vector<uint8_t>> &BlockData::rgba(uint32_t width, uint32_t height) const {
    std::call_once(once, [&] {
      decoded = decode(width, height, uint32_t(offsets.size() - 1), true, block, false, data.data(), data.size()).mips;
      if (opaqueAlpha)
        for (auto &mip : decoded)
          for (size_t i = 3; i < mip.size(); i += 4)
            mip[i] = 255;
    });
    return decoded;
  }

  bool Pixels::sameContent(const Pixels &other) const {
    if (width != other.width || height != other.height || srgb != other.srgb || twoChannel != other.twoChannel ||
        bool(blocks) != bool(other.blocks))
      return false;
    if (blocks)
      return blocks == other.blocks ||
             (blocks->block == other.blocks->block && blocks->opaqueAlpha == other.blocks->opaqueAlpha &&
              blocks->offsets == other.blocks->offsets && blocks->data == other.blocks->data);
    return mips == other.mips && hdrMips == other.hdrMips;
  }

  Pixels decodeTexture(const remixapi_TextureInfo &info) {
    if (info.depth > 1 || info.dataSize > SIZE_MAX) {
      throw invalid;
    }
    uint32_t block = 0;
    bool srgb = false, bgra = false, bc1Alpha = false;
    switch (uint32_t(info.format)) {
    case REMIXAPI_FORMAT_R8G8B8A8_SRGB:
      srgb = true;
      break;
    case REMIXAPI_FORMAT_R8G8B8A8_UNORM:
      break;
    case REMIXAPI_FORMAT_B8G8R8A8_SRGB:
      srgb = true;
      bgra = true;
      break;
    case REMIXAPI_FORMAT_B8G8R8A8_UNORM:
      bgra = true;
      break;
    case REMIXAPI_FORMAT_BC1_RGB_SRGB:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC1_RGB_UNORM:
      block = 1;
      break;
    case REMIXAPI_FORMAT_BC1_RGBA_SRGB_EXT:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC1_RGBA_UNORM_EXT:
      block = 1;
      bc1Alpha = true;
      break;
    case REMIXAPI_FORMAT_BC3_SRGB:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC3_UNORM:
      block = 3;
      break;
    case REMIXAPI_FORMAT_BC4_UNORM_EXT:
      block = 4;
      break;
    case REMIXAPI_FORMAT_BC6H_UFLOAT_EXT:
      block = 6;
      break;
    case REMIXAPI_FORMAT_BC6H_SFLOAT_EXT:
      block = 16;
      break;
    case REMIXAPI_FORMAT_BC5_UNORM:
      block = 5;
      break;
    case REMIXAPI_FORMAT_BC7_SRGB:
      srgb = true;
      [[fallthrough]];
    case REMIXAPI_FORMAT_BC7_UNORM:
      block = 7;
      break;
    default:
      throw unavailable;
    }
    auto result = decode(info.width, info.height, info.mipLevels, srgb, block, bgra,
                         static_cast<const uint8_t *>(info.data), size_t(info.dataSize), true,
                         block == 1 && !bc1Alpha);
    // The raw API exposes BC1 RGB, whereas DDS DXT1 can also carry one-bit alpha.
    if (block == 1 && !bc1Alpha) {
      for (auto &mip : result.mips) {
        for (size_t i = 3; i < mip.size(); i += 4) {
          mip[i] = 255;
        }
      }
    }
    return result;
  }

  Pixels loadTexture(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
      throw REMIXAPI_ERROR_CODE_GENERAL_FAILURE;
    }
    const auto length = file.tellg();
    if (length <= 0 || length > 512 * 1024 * 1024) {
      throw invalid;
    }
    std::vector<uint8_t> data(static_cast<size_t>(length));
    file.seekg(0);
    if (!file.read(reinterpret_cast<char *>(data.data()), length)) {
      throw invalid;
    }
    if (data.size() >= 128 && word(data, 0) == 0x20534444) {
      if (word(data, 4) != 124 || word(data, 76) != 32 || word(data, 24) > 1 ||
          (word(data, 112) & 0x20fe00)) {
        throw invalid;
      } // No volumes or cube faces.
      uint32_t width = word(data, 16), height = word(data, 12), block = 0;
      bool bgra = false, srgb = true, forceAlpha = false;
      size_t offset = 128;
      const uint32_t fourCC = word(data, 84);
      unsigned floatChannels = 0;
      bool floatHalf = false;
      if (word(data, 80) & 4) {
        if (fourCC >= 111 && fourCC <= 116) {
          floatHalf = fourCC <= 113;
          const auto n = (fourCC - 111) % 3;
          floatChannels = n == 0 ? 1 : n == 1 ? 2 : 4;
          srgb = false;
        } else if (fourCC == 0x31545844) {
          block = 1;
        } else if (fourCC == 0x33545844) {
          block = 2;
        } else if (fourCC == 0x35545844) {
          block = 3;
        } else if (fourCC == 0x31495441 || fourCC == 0x55344342) {
          block = 4;
          srgb = false;
        } else if (fourCC == 0x32495441 || fourCC == 0x55354342) {
          block = 5;
          srgb = false;
        } else if (fourCC == 0x30315844) {
          if (data.size() < 148 || word(data, 132) != 3 || word(data, 140) != 1 || (word(data, 136) & 4)) {
            throw invalid;
          }
          offset = 148;
          switch (word(data, 128)) {
          case 2:
            floatChannels = 4;
            break;
          case 6:
            floatChannels = 3;
            break;
          case 10:
            floatChannels = 4;
            floatHalf = true;
            break;
          case 16:
            floatChannels = 2;
            break;
          case 34:
            floatChannels = 2;
            floatHalf = true;
            break;
          case 41:
            floatChannels = 1;
            break;
          case 54:
            floatChannels = 1;
            floatHalf = true;
            break;
          case 28:
          case 29:
            break;
          case 87:
          case 91:
            bgra = true;
            break;
          case 71:
          case 72:
            block = 1;
            break;
          case 74:
          case 75:
            block = 2;
            break;
          case 77:
          case 78:
            block = 3;
            break;
          case 80:
            block = 4;
            srgb = false;
            break;
          case 95:
            block = 6;
            srgb = false;
            break;
          case 96:
            block = 16;
            srgb = false;
            break;
          case 83:
            block = 5;
            srgb = false;
            break;
          case 98:
          case 99:
            block = 7;
            break;
          default:
            throw unavailable;
          }
        } else {
          throw unavailable;
        }
      } else {
        if (!(word(data, 80) & 0x40) || word(data, 88) != 32 || word(data, 96) != 0xff00 ||
            (word(data, 104) != 0xff000000 && word(data, 104) != 0)) {
          throw unavailable;
        }
        const auto r = word(data, 92), b = word(data, 100);
        if (r == 0xff0000 && b == 0xff) {
          bgra = true;
        } else if (r != 0xff || b != 0xff0000) {
          throw unavailable;
        }
        forceAlpha = word(data, 104) == 0;
        // This subset uses tightly packed 32-bit rows.
        if ((word(data, 8) & 8) && word(data, 20) != width * 4ull) {
          throw unavailable;
        }
      }
      if (floatChannels)
        return decodeFloat(width, height, word(data, 28), floatChannels, floatHalf, data.data() + offset,
                           data.size() - offset);
      auto out = decode(width, height, word(data, 28), srgb, block, bgra, data.data() + offset,
                        data.size() - offset, true);
      if (forceAlpha) {
        for (auto &mip : out.mips) {
          for (size_t i = 3; i < mip.size(); i += 4) {
            mip[i] = 255;
          }
        }
      }
      return out;
    }
    int width = 0, height = 0, channels = 0;
    if (!stbi_info_from_memory(data.data(), int(data.size()), &width, &height, &channels) || width <= 0 ||
        height <= 0 || uint64_t(width) * height * 4 > 512u * 1024 * 1024) {
      throw invalid;
    }
    if (stbi_is_hdr_from_memory(data.data(), int(data.size()))) {
      std::unique_ptr<float, decltype(&stbi_image_free)> hdr(
          stbi_loadf_from_memory(data.data(), int(data.size()), &width, &height, &channels, 4),
          stbi_image_free);
      if (!hdr || uint64_t(width) * height * 20 > 512u * 1024 * 1024)
        throw invalid;
      Pixels out{
          uint32_t(width), uint32_t(height), false, {std::vector<uint8_t>(size_t(width) * height * 4)}};
      out.hdrMips.emplace_back(hdr.get(), hdr.get() + size_t(width) * height * 4);
      for (size_t i = 0; i < out.mips[0].size(); ++i)
        out.mips[0][i] = uint8_t(std::clamp(hdr.get()[i], 0.f, 1.f) * 255);
      return out;
    }
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(data.data(), int(data.size()), &width, &height, &channels, 4), stbi_image_free);
    if (!pixels) {
      throw invalid;
    }
    return decode(uint32_t(width), uint32_t(height), 1, true, 0, false, pixels.get(),
                  size_t(width) * height * 4);
  }
} // namespace remix::scene
