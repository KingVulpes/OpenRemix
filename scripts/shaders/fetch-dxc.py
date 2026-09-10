#!/usr/bin/env python3
"""Fetch the pinned optional Linux x64 shader compiler (Python 3.12+)."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--output', type=Path, required=True, help='Fresh compiler directory')
args = parser.parse_args()
target = args.output.resolve()
if target.exists():
    parser.error('Use a fresh output directory')
lock = json.loads(Path(__file__).with_name('toolchain.lock.json').read_text())['dxc']
asset = lock['linux-x86_64']
target.parent.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(dir=target.parent) as temporary:
    archive = Path(temporary) / 'dxc.tar.gz'
    with urllib.request.urlopen(asset['url'], timeout=60) as response, archive.open('wb') as output:
        shutil.copyfileobj(response, output)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != asset['sha256']:
        raise SystemExit('DXC archive checksum mismatch')
    unpacked = Path(temporary) / 'compiler'
    with tarfile.open(archive) as package:
        package.extractall(unpacked, filter='data')
    version = subprocess.check_output([str(unpacked / 'bin/dxc'), '--version'], text=True)
    if lock['version'] not in version:
        raise SystemExit('DXC version mismatch')
    unpacked.rename(target)
print('Set REMIX_DXC=' + str(target / 'bin/dxc'))
