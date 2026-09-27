#pragma once
#include "pathtrace/options.h"
#include <array>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace remix::config {
  struct Settings {
    TraceOptions trace;
    uint32_t backend = 0;
    bool vsync = false;
    std::array<float, 4> clearColor = {0.025f, 0.035f, 0.055f, 1.f};
    // Upload BC textures as GPU BC images instead of decoding them on the CPU.
    // Applies to textures created after the change; no restart needed.
    bool compressedTextures = false;
    // Fill GetVramStats from OpenRemix's allocations and the driver budget.
    bool vramStatistics = false;
    bool operator==(const Settings &) const = default;
  };
  using UnknownSettings = std::map<std::string, std::string>;
  enum class SetResult { Success, Unknown, Invalid };
  SetResult set(Settings &settings, std::string_view key, std::string_view value);
  bool validate(const Settings &settings, std::string &error);
  std::string serialize(const Settings &settings, const UnknownSettings &unknown = {});
  // Transactional: all output arguments stay untouched on failure.
  bool parse(std::string_view text, Settings &settings, UnknownSettings &unknown,
             std::vector<std::string> &warnings, std::string &error);
} // namespace remix::config
