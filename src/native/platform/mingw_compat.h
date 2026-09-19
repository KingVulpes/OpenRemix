#pragma once

// MSVC exposes floating-point abs overloads in the global namespace; MinGW's
// C++ headers keep them in std. Supply the same overload set for the pinned
// MathLib headers without modifying vendor sources.
#if defined(__MINGW32__)
#include <cmath>
using std::abs;
#endif
