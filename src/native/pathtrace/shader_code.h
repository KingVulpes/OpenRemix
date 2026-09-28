#pragma once
#include "pathtrace/shaders/embedded.h"
#include <cstddef>
#include <cstdint>

namespace remix::trace {
  // SPIR-V of an embedded stage.
  struct ShaderCode {
    const uint32_t *code = nullptr;
    size_t bytes = 0;
  };
  // Decompressed on first use, from any thread, and kept for the process
  // lifetime: the pointer stays valid for lazily compiled variants.
  ShaderCode shader(Stage stage);
} // namespace remix::trace
