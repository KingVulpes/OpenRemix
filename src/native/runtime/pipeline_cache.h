#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace remix::pipeline {
struct CacheIdentity {
  uint32_t vendor{}, device{}, driver{};
  std::array<uint8_t, 16> uuid{};
};
std::vector<uint8_t> readCache(const std::filesystem::path &file,
                               const CacheIdentity &identity) noexcept;
bool writeCache(const std::filesystem::path &file,
                const CacheIdentity &identity,
                std::span<const uint8_t> data) noexcept;
} // namespace remix::pipeline
