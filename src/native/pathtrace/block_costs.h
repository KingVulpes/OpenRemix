#pragma once
#include <cstdint>

namespace remix::trace {
  // The slowest destruction of a buffer since this was last read (diagnostics):
  // its size, and whether it freed memory of its own; and the slowest freeing
  // of a memory block.
  inline double g_slowestBufferDestroy = 0, g_slowestBlockFree = 0;
  inline uint64_t g_slowestBufferDestroyBytes = 0;
  inline bool g_slowestBufferDestroyOwn = false;
  // Sparse rendering in the last traced frame (diagnostics): whether the
  // opaque pass was launched over the selected pixels, the share selected,
  // and otherwise why not (a string literal).
  struct SparseState {
    bool requested = false, launched = false;
    float rate = 1;
    const char *reason = "";
  };
  inline SparseState g_sparseState;
} // namespace remix::trace
