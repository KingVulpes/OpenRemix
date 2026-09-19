#pragma once
#include <chrono>

namespace remix {
  struct ScopedMilliseconds {
    double &total;
    std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
    ~ScopedMilliseconds() {
      total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
    }
  };
  template<class F> auto measured(double &total, F &&call) {
    ScopedMilliseconds time{total};
    return call();
  }
}
