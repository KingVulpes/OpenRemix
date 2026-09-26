// Static capture composition adapted from the Godot toolkit. CPU-only OpenUSD 25.11.
#pragma once
#include <nlohmann/json.hpp>

namespace remix::usd {
  // Names and enums match capture primvars and InstanceInfoBlendEXT. Defaults
  // follow Remix's LegacyMaterialData, with standard alpha blend factors when
  // incomplete captures enable blending without specifying their factors.
  inline nlohmann::json capture_alpha_defaults() {
    return {{"alphaTestEnabled", false},
            {"alphaTestReferenceValue", 0u},
            {"alphaTestCompareOp", 7u},
            {"alphaBlendEnabled", false},
            {"srcColorBlendFactor", 6u},
            {"dstColorBlendFactor", 7u},
            {"colorBlendOp", 0u},
            {"srcAlphaBlendFactor", 1u},
            {"dstAlphaBlendFactor", 7u},
            {"alphaBlendOp", 0u},
            {"writeMask", 15u},
            {"textureColorArg1Source", 1u},
            {"textureColorArg2Source", 0u},
            {"textureColorOperation", 3u},
            {"textureAlphaArg1Source", 1u},
            {"textureAlphaArg2Source", 0u},
            {"textureAlphaOperation", 1u},
            {"tFactor", 0xffffffffu},
            {"isTextureFactorBlend", false},
            {"isVertexColorBakedLighting", true}};
  }
} // namespace remix::usd
