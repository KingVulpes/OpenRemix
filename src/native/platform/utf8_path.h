#pragma once
#include <filesystem>
#include <string_view>

namespace remix {
  // UTF-8 text as a filesystem path. std::filesystem::u8path is deprecated in
  // C++20, but it also works with the GCC 8 standard library used by the
  // glibc 2.28 build, which lacks the char8_t path constructors.
  inline std::filesystem::path utf8Path(std::string_view text) {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    return std::filesystem::u8path(text.begin(), text.end());
#pragma GCC diagnostic pop
  }
} // namespace remix
