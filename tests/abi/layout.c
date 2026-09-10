#include <remix/remix_native_ext.h>
#if defined(_WIN32)
#include "windows_layout.h"
#else
#include "posix_layout.h"
#endif
_Static_assert(REMIXAPI_VERSION_MINOR == 1003, "ABI line");
_Static_assert(sizeof(remixapi_CameraEnvironmentEXT) == 32, "Camera environment ABI");
_Static_assert(offsetof(remixapi_CameraEnvironmentEXT, enabled) == 16, "Camera environment enabled");
_Static_assert(offsetof(remixapi_CameraEnvironmentEXT, radiance) == 20, "Camera environment radiance");
_Static_assert(sizeof(remixapi_SharcStatisticsEXT) == 344, "SHARC statistics ABI");
_Static_assert(offsetof(remixapi_SharcStatisticsEXT, memoryBytes) == 48, "SHARC statistics alignment");
int main(void) { return 0; }
