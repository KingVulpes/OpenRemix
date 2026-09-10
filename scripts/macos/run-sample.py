#!/usr/bin/env python3
"""Run a packaged shell using its standard SDK MoltenVK."""
import argparse
from pathlib import Path
import subprocess
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--package', type=Path, required=True)
parser.add_argument('--frames', type=int, default=300)
args = parser.parse_args()
subprocess.run([str(args.package.resolve() / 'bin/run-sample'), str(args.frames)], check=True, timeout=90)
