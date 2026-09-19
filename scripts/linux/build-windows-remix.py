#!/usr/bin/env python3
"""Cross-build Windows x64 with Clang/MinGW and run CPU tests through Wine."""
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import urllib.request


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', type=Path, default=root / '_work/build/windows-cross')
    parser.add_argument('--deps', type=Path, default=root / '_work/deps/windows')
    parser.add_argument('--cmake', default='cmake')
    parser.add_argument('--jobs', type=int, default=8)
    parser.add_argument('--skip-tests', action='store_true')
    args = parser.parse_args()
    build, deps = args.build.resolve(), args.deps.resolve()
    prefix = deps / 'prefix'
    for tool in ('clang', 'clang++', 'llvm-ar', 'llvm-dlltool',
                 'x86_64-w64-mingw32-g++', 'x86_64-w64-mingw32-windres',
                 'meson', 'ninja', 'pkg-config', 'wine', args.cmake):
        if not shutil.which(tool):
            parser.error('Missing build tool: ' + tool)
    deps.mkdir(parents=True, exist_ok=True)
    (prefix / 'lib/pkgconfig').mkdir(parents=True, exist_ok=True)
    (prefix / 'bin').mkdir(parents=True, exist_ok=True)

    # Only an import library is needed: Windows/Proton provides vulkan-1.dll.
    # Match the repository's pinned Vulkan 1.4.309 headers.
    definition = deps / 'vulkan-1.def'
    digest = '9ba339b7f5ee2df28487698a6840ecf095fac58b415c2c89ad9f161e9d316378'
    if not definition.exists() or hashlib.sha256(definition.read_bytes()).hexdigest() != digest:
        data = urllib.request.urlopen(
            'https://raw.githubusercontent.com/KhronosGroup/Vulkan-Loader/v1.4.309/loader/vulkan-1.def').read()
        if hashlib.sha256(data).hexdigest() != digest:
            raise RuntimeError('Vulkan import definition checksum mismatch')
        definition.write_bytes(data)
    subprocess.run(['llvm-dlltool', '-m', 'i386:x86-64', '-d', str(definition),
                    '-l', str(prefix / 'lib/libvulkan-1.dll.a')], check=True)
    (prefix / 'lib/pkgconfig/vulkan.pc').write_text(
        f'Name: Vulkan\nDescription: Windows Vulkan loader import library\nVersion: 1.4.309\n'
        f'Libs: -L{prefix}/lib -lvulkan-1\nCflags: -I{root}/include/vulkan/include\n')

    spec = importlib.util.spec_from_file_location('dependencies', root / 'scripts/common/build-dependencies.py')
    sys.dont_write_bytecode = True
    dependency_builder = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(dependency_builder)
    pin = json.loads((root / 'scripts/common/dependencies.lock.json').read_text())['dependencies']['sdl3']
    sdl_source = dependency_builder.fetch(deps, 'sdl3', pin)
    target = 'x86_64-w64-windows-gnu'
    toolchain = deps / 'cmake-windows.cmake'
    toolchain.write_text(
        'set(CMAKE_SYSTEM_NAME Windows)\nset(CMAKE_SYSTEM_PROCESSOR x86_64)\n'
        'set(CMAKE_C_COMPILER clang)\nset(CMAKE_CXX_COMPILER clang++)\n'
        f'set(CMAKE_C_COMPILER_TARGET {target})\nset(CMAKE_CXX_COMPILER_TARGET {target})\n'
        'set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)\n'
        'set(CMAKE_FIND_ROOT_PATH /usr/x86_64-w64-mingw32)\n'
        'set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)\n'
        'set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)\n'
        'set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)\n'
        'set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)\n')
    subprocess.run([args.cmake, '-S', str(sdl_source), '-B', str(deps / 'build-sdl3'), '-G', 'Ninja',
        '-DCMAKE_TOOLCHAIN_FILE=' + str(toolchain), '-DCMAKE_BUILD_TYPE=Release',
        '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DCMAKE_INSTALL_LIBDIR=lib',
        '-DSDL_SHARED=ON', '-DSDL_STATIC=OFF', '-DSDL_TEST_LIBRARY=OFF',
        '-DSDL_TESTS=OFF', '-DSDL_EXAMPLES=OFF'], check=True)
    subprocess.run([args.cmake, '--build', str(deps / 'build-sdl3'), '--parallel', str(args.jobs)], check=True)
    subprocess.run([args.cmake, '--install', str(deps / 'build-sdl3')], check=True)

    # Shared MinGW dependencies are placed beside SDL and packaged with remix.dll.
    for name in ('libwinpthread-1.dll', 'libgcc_s_seh-1.dll', 'libstdc++-6.dll'):
        source = Path(subprocess.check_output(['x86_64-w64-mingw32-g++', '-print-file-name=' + name], text=True).strip())
        if not source.is_file():
            import_library = Path(subprocess.check_output(
                ['x86_64-w64-mingw32-g++', '-print-file-name=libwinpthread.dll.a'], text=True).strip())
            source = import_library.parent.parent / 'bin' / name
        if not source.is_file():
            raise RuntimeError('Cannot find MinGW runtime ' + name)
        shutil.copy2(source, prefix / 'bin' / name)

    wrapper = deps / 'wine-run.sh'
    wine_paths = ';'.join('Z:' + str(p).replace('/', '\\') for p in (prefix / 'bin', build / 'src/native'))
    wrapper.write_text('#!/usr/bin/env bash\nset -euo pipefail\n' +
        'export WINEPREFIX=' + shlex.quote(str(deps / 'wine-prefix')) + '\n' +
        'export WINEPATH=' + shlex.quote(wine_paths) + '\n' +
        'export WINEDEBUG=-all\nexport WINEDLLOVERRIDES="mscoree,mshtml="\n' + r'''args=()
for arg in "$@"; do
  if [[ "$arg" == /* ]]; then arg="Z:${arg//\//\\}"; fi
  args+=("$arg")
done
exec wine "${args[@]}"
''')
    wrapper.chmod(0o755)
    cross = deps / 'windows-cross.ini'
    cross.write_text(f'''[binaries]
c = ['clang', '--target={target}']
cpp = ['clang++', '--target={target}']
ar = 'llvm-ar'
strip = 'llvm-strip'
windres = 'x86_64-w64-mingw32-windres'
pkg-config = 'pkg-config'
exe_wrapper = '{wrapper}'

[host_machine]
system = 'windows'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '{prefix}/lib/pkgconfig'

[built-in options]
c_link_args = ['-static-libgcc']
cpp_link_args = ['-static-libgcc', '-pthread']
''')
    setup = ['meson', 'setup', str(build), str(root), '--cross-file', str(cross),
             '-Ddeps_root=' + str(prefix), '-Dbuildtype=release', '-Dwerror=true',
             '-Dc_link_args=-static-libgcc', "-Dcpp_link_args=['-static-libgcc', '-pthread']"]
    if (build / 'meson-private/coredata.dat').exists():
        setup += ['--reconfigure']
    subprocess.run(setup, check=True)
    subprocess.run(['meson', 'compile', '-C', str(build), '-j', str(args.jobs)], check=True)
    if not args.skip_tests:
        subprocess.run(['meson', 'test', '-C', str(build), '--print-errorlogs', '--num-processes', '1'], check=True)
    print('Windows runtime:', build / 'src/native/remix.dll')
    print('Runtime dependencies:', prefix / 'bin')


if __name__ == '__main__':
    main()
