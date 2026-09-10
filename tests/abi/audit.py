"""Check retained headers, ImGui pin, exports and absence of renderer dependencies."""
import ctypes
import hashlib
from pathlib import Path
import subprocess
import sys

root, library = Path(sys.argv[1]), Path(sys.argv[2])
for manifest, base in [(root / 'tests/abi/headers.sha256', root),
                       (root / 'third_party/imgui/SHA256SUMS', root / 'third_party/imgui'),
                       (root / 'third_party/image/SHA256SUMS', root / 'third_party/image'),
                       (root / 'third_party/nrd/sources.sha256', root / 'third_party/nrd'),
                       (root / 'third_party/sharc/sources.sha256', root / 'third_party/sharc')]:
    for line in manifest.read_text().splitlines():
        checksum, filename = line.split('  ', 1)
        assert hashlib.sha256((base / filename).read_bytes()).hexdigest() == checksum, filename
runtime = ctypes.CDLL(str(library), **({'winmode': 0} if sys.platform == 'win32' else {}))
for name in (root / 'tests/abi/exports.txt').read_text().splitlines():
    assert getattr(runtime, name), name
for name in ('remixapi_GetBackendCapabilitiesEXT', 'remixapi_GetTracingStatisticsEXT',
             'remixapi_GetLocalLightStatisticsEXT', 'remixapi_GetShadowStatisticsEXT', 'remixapi_GetSharcStatisticsEXT', 'remixapi_GetDenoisingCapabilitiesEXT', 'remixapi_GetDenoisingStatisticsEXT'):
    assert getattr(runtime, name), name
assert runtime.remixapi_imgui_Begin(None, None, 0) == 0
value = ctypes.c_float(12)
runtime.remixapi_imgui_GetWindowSize(ctypes.byref(value), None)
assert value.value == 0
if sys.platform.startswith('linux'):
    dynamic = subprocess.check_output(['readelf', '-d', library], text=True)
    needed = '\n'.join(line.lower() for line in dynamic.splitlines() if '(NEEDED)' in line)
    for removed in ['ngx', 'nrd', 'usd', 'rtxio', 'dxvk', 'd3d', 'xess', 'slang']:
        assert removed not in needed, removed
    symbols = subprocess.check_output(['nm', '-D', '--undefined-only', library], text=True)
    assert 'vkGetBufferDeviceAddress' not in symbols, 'Tracing must not require a Vulkan 1.2 loader for raster'
print('Header checksums, ImGui pin, required exports, inert widgets and dependency audit passed')
