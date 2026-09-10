#!/usr/bin/env python3
"""Deploy and run an instance-local Vulkan probe over ADB without root."""
import argparse
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--serial', help='ADB serial; required if more than one device is connected')
parser.add_argument('--build', type=Path, default=root / '_work/build/android-probe')
parser.add_argument('--mode', choices=['query', 'bindless', 'pipeline', 'semaphore'], default='query')
parser.add_argument('--output', type=Path, default=root / '_work/reports/android-probe')
args = parser.parse_args()
adb = ['adb'] + (['-s', args.serial] if args.serial else [])
remote = '/data/local/tmp/remix-vulkan-probe'
args.output.mkdir(parents=True, exist_ok=True)
subprocess.run(adb + ['shell', 'mkdir', '-p', remote], check=True)
for source in [args.build / 'remix-vulkan-probe', *args.build.glob('*.spv')]:
    subprocess.run(adb + ['push', str(source), remote + '/'], check=True)
subprocess.run(adb + ['shell', 'chmod', '755', remote + '/remix-vulkan-probe'], check=True)
result = subprocess.run(adb + ['shell', 'timeout', '30', remote + '/remix-vulkan-probe', remote, args.mode],
    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=40)
(args.output / (args.mode + '.log')).write_text(result.stdout)
print(result.stdout, end='')
raise SystemExit(result.returncode)
