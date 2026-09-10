#!/usr/bin/env python3
"""Cross-compile the headless Vulkan ray tracing probe for arm64 Android."""
import argparse
from pathlib import Path
import shutil
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--ndk', type=Path, default=root / '_work/deps/android/ndk-r29')
parser.add_argument('--glslang', default=shutil.which('glslangValidator'))
parser.add_argument('--output', type=Path, default=root / '_work/build/android-probe')
args = parser.parse_args()
compilers = list(args.ndk.glob('toolchains/llvm/prebuilt/*/bin/aarch64-linux-android29-clang++'))
if len(compilers) != 1:
    parser.error('Provide --ndk with one host toolchain for Android NDK r29.')
if not args.glslang:
    parser.error('Provide --glslang pointing to the host Vulkan SDK glslangValidator.')
out = args.output.resolve()
out.mkdir(parents=True, exist_ok=True)
subprocess.run([str(compilers[0]), '-std=c++17', '-O2', '-Wall', '-Wextra',
    '-Wno-missing-field-initializers', '-static-libstdc++',
    '-I' + str(root / 'include/vulkan/include'),
    str(root / 'tools/vulkan-probes/remix_vulkan_probe.cpp'), '-lvulkan',
    '-o', str(out / 'remix-vulkan-probe')], check=True)
for name in ('probe.comp', 'probe.rgen', 'probe.rmiss', 'probe.rchit'):
    subprocess.run([args.glslang, '-V', '--target-env', 'vulkan1.2',
        str(root / 'tools/vulkan-probes/shaders' / name), '-o', str(out / (name + '.spv'))], check=True)
subprocess.run([args.glslang, '-V', '--target-env', 'vulkan1.2', '-DUSE_BINDLESS=1',
    str(root / 'tools/vulkan-probes/shaders/probe.comp'), '-o', str(out / 'probe_bindless.comp.spv')], check=True)
print(out)
