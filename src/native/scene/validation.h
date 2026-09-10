#pragma once
#include "scene/types.h"
#include <remix/remix_native_ext.h>

namespace remix::scene::detail {
  constexpr auto invalid = REMIXAPI_ERROR_CODE_INVALID_ARGUMENTS;
  constexpr auto unavailable = REMIXAPI_ERROR_CODE_FEATURE_UNAVAILABLE;
  template <typename T> uint64_t key(T handle) { return reinterpret_cast<uintptr_t>(handle); }
  void finite(const float *values, size_t count);
  void rgba(uint32_t color, float *out, bool ui = false);
  Blend blend(const remixapi_InstanceInfoBlendEXT *info);
} // namespace remix::scene::detail
