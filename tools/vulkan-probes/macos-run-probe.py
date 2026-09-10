#!/usr/bin/env python3
"""Build and run the isolated ray tracing compatibility gate on macOS."""

import argparse
import json
import os
from pathlib import Path
import subprocess


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--vulkan-sdk', type=Path, required=True, help='Vulkan SDK macOS directory')
    parser.add_argument('--deps', type=Path, default=root / '_work/deps/macos', help='SDL3 dependency prefix owner')
    parser.add_argument('--driver', type=Path, default=root / '_work/deps/moltenvk', help='Experimental MoltenVK build output')
    parser.add_argument('--build', type=Path, default=root / '_work/build/macos-probe')
    parser.add_argument('--allow-experimental-gpu-run', action='store_true',
                        help='Opt in to the experimental driver after the full renderer caused a host reboot')
    args = parser.parse_args()
    if not args.allow_experimental_gpu_run:
        parser.error('GPU runs are disabled following a host reboot during the full renderer test. '
                     'See docs/history/native-macos-blocker.md; use --allow-experimental-gpu-run only for deliberate driver testing.')
    sdk = args.vulkan_sdk.resolve()
    build = args.build.resolve()
    (build / 'probe-result.json').unlink(missing_ok=True)
    icd = args.driver.resolve() / 'prefix/etc/vulkan/icd.d/MoltenVK_icd.json'
    if not icd.is_file():
        parser.error('Build the experimental driver with tools/moltenvk/build-moltenvk.py first.')
    if not (sdk / 'share/vulkan/explicit_layer.d/VkLayer_khronos_validation.json').is_file():
        parser.error('The SDK must include Vulkan validation layers.')
    env = dict(os.environ)
    env['PATH'] = str(sdk / 'bin') + os.pathsep + env.get('PATH', '')
    env['PKG_CONFIG_PATH'] = str(sdk / 'lib/pkgconfig')
    env['VK_DRIVER_FILES'] = str(icd)
    env['VK_LAYER_PATH'] = str(sdk / 'share/vulkan/explicit_layer.d')
    env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_KHRONOS_validation'
    env['MVK_CONFIG_ENABLE_EXPERIMENTAL_RAY_TRACING'] = '1'
    env['MVK_CONFIG_LOG_LEVEL'] = '1'
    env['MTL_DEBUG_LAYER'] = '1'
    command = ['meson', 'setup', str(build), str(root), '-Dvulkan_probes=true', '-Dgpu_tests=true', '-Denable_tests=false', '-Ddeps_root=' + str(args.deps.resolve() / 'prefix')]
    if (build / 'meson-private/coredata.dat').exists():
        command.append('--reconfigure')
    subprocess.run(command, env=env, check=True)
    subprocess.run(['meson', 'compile', '-C', str(build)], env=env, check=True)
    subprocess.run(['meson', 'test', '-C', str(build), '--print-errorlogs', '--num-processes', '1'], env=env, check=True)
    result = {
        'driver': json.loads((root / 'tools/moltenvk/dependencies.lock.json').read_text()),
        'os': subprocess.check_output(['sw_vers'], text=True),
        'gpu': subprocess.check_output(['sysctl', '-n', 'machdep.cpu.brand_string'], text=True).strip(),
        'xcode': subprocess.check_output(['xcodebuild', '-version'], text=True),
        'sdk': str(sdk),
        'vulkan_validation': True,
        'metal_validation': True,
        'result': 'passed',
    }
    (build / 'probe-result.json').write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
