#include <remix/remix_c.h>
#if defined(_WIN32)
#include "windows_layout.h"
#else
#include "posix_layout.h"
#endif
_Static_assert(REMIXAPI_VERSION_MINOR == 1003, "ABI line");
int main(void) { return 0; }
