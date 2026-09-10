#!/usr/bin/env python3
"""Build the pinned experimental driver without changing the installed Vulkan SDK."""

import argparse
import json
from pathlib import Path
import platform
import subprocess


def run(*args, **kwargs):
    subprocess.run([str(arg) for arg in args], check=True, **kwargs)


def main():
    root = Path(__file__).resolve().parents[2]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=Path, default=root / '_work/deps/moltenvk')
    parser.add_argument('--jobs', type=int, default=6)
    args = parser.parse_args()
    if platform.system() != 'Darwin' or platform.machine() != 'arm64':
        parser.error('This experimental build targets Apple Silicon macOS.')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    lock = json.loads((root / 'tools/moltenvk/dependencies.lock.json').read_text())
    pin = lock['dependencies']['moltenvk']
    source = output / 'MoltenVK'
    if not source.exists():
        run('git', 'clone', '--no-checkout', pin['repository'], source)
        run('git', '-C', source, 'checkout', '--detach', pin['revision'])
    revision = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
    if revision != pin['revision']:
        parser.error(f'{source} is at {revision}, expected {pin["revision"]}; use a fresh output directory')
    cross_revision = (source / 'ExternalRevisions/SPIRV-Cross_repo_revision').read_text().strip()
    if cross_revision != lock['dependencies']['spirv-cross']['revision']:
        parser.error('MoltenVK and SPIRV-Cross pins do not match')

    for patch in sorted((root / 'tools/moltenvk/patches').glob('moltenvk-*.patch')):
        applied = subprocess.run(['git', '-C', str(source), 'apply', '--reverse', '--check', str(patch)],
                                 stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
        if not applied:
            run('git', '-C', source, 'apply', '--check', patch)
            run('git', '-C', source, 'apply', patch)
    prefix = output / 'prefix'
    build = output / 'build-moltenvk'
    run('cmake', '-S', source, '-B', build, '-G', 'Ninja',
        '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_OSX_ARCHITECTURES=arm64',
        '-DCMAKE_OSX_DEPLOYMENT_TARGET=' + lock['deployment_target'],
        '-DCMAKE_INSTALL_PREFIX=' + str(prefix), '-DMVK_BUILD_SHADER_CONVERTER_TOOL=ON')
    run('cmake', '--build', build, '--parallel', args.jobs, '--target', 'MoltenVK', 'MoltenVKShaderConverter')
    run('cmake', '--install', build)
    icd = prefix / 'etc/vulkan/icd.d/MoltenVK_icd.json'
    manifest = json.loads(icd.read_text())
    manifest['ICD']['library_path'] = '../../../lib/libMoltenVK.dylib'
    icd.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Experimental ICD: {icd}')


if __name__ == '__main__':
    main()
