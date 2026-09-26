// Static capture composition adapted from the Godot toolkit. CPU-only OpenUSD 25.11.
#pragma once
#include <map>
#include <nlohmann/json.hpp>
#include <remix/remix_c.h>
#include <vector>

namespace remix::usd {
  // Remix has category flags per instance, while USD can bind a different
  // material to each face subset. Keep those surfaces in separate draw calls.
  inline std::map<uint32_t, std::vector<size_t>> category_surface_groups(const nlohmann::json &mesh) {
    uint32_t fallback = mesh.value("category_flags", 0u);
    if (!mesh.contains("category_flags")) {
      if (mesh.value("ignore_alpha", false))
        fallback |= REMIXAPI_INSTANCE_CATEGORY_BIT_IGNORE_ALPHA_CHANNEL;
      if (mesh.value("alpha_blend_to_cutout", false))
        fallback |= REMIXAPI_INSTANCE_CATEGORY_BIT_ALPHA_BLEND_TO_CUTOUT;
    }
    std::map<uint32_t, std::vector<size_t>> groups;
    const auto &surfaces = mesh.at("surfaces");
    for (size_t i = 0; i < surfaces.size(); ++i)
      groups[surfaces[i].value("category_flags", fallback)].push_back(i);
    return groups;
  }
} // namespace remix::usd
