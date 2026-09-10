#!/usr/bin/env python3
"""Cross-build the arm64 Android native Remix presentation shell."""
import argparse
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ndk', type=Path, default=root / '_work/deps/android/ndk-r29')
parser.add_argument('--deps', type=Path, default=root / '_work/deps/android/prefix')
parser.add_argument('--build', type=Path, default=root / '_work/build/android')
parser.add_argument('--jobs', type=int, default=8)
args = parser.parse_args()
prebuilt = list(args.ndk.resolve().glob('toolchains/llvm/prebuilt/*/bin'))
if len(prebuilt) != 1:
    parser.error('Provide --ndk pointing to an installed NDK r29.')
tools = prebuilt[0]
build = args.build.resolve()
build.mkdir(parents=True, exist_ok=True)
def quoted(value):
    return "'" + str(value).replace('\\', '\\\\').replace("'", "\\'") + "'"
cross = build / 'android-arm64.ini'
cross.write_text('[binaries]\n' + '\n'.join(key + ' = ' + quoted(tools / value) for key, value in {
    'c': 'aarch64-linux-android29-clang', 'cpp': 'aarch64-linux-android29-clang++',
    'ar': 'llvm-ar', 'strip': 'llvm-strip'}.items()) + '''
[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[properties]
needs_exe_wrapper = true
[built-in options]
cpp_link_args = ['-static-libstdc++']
''')
command = ['meson', 'setup', str(build), str(root), '--cross-file', str(cross),
    '-Dbuildtype=release', '-Denable_tests=true',
    '-Ddeps_root=' + str(args.deps.resolve())]
if (build / 'meson-private/coredata.dat').exists():
    command.append('--reconfigure')
subprocess.run(command, check=True)
subprocess.run(['meson', 'compile', '-C', str(build), '-j', str(args.jobs)], check=True)
