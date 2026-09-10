#!/usr/bin/env python3
"""Install and run the shell sample on a connected Android device."""
import argparse
from pathlib import Path
import subprocess
import time
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--serial')
parser.add_argument('--apk', type=Path, default=Path('_work/packages/android/remix-sample.apk'))
parser.add_argument('--frames', type=int, default=600)
parser.add_argument('--timeout', type=int, default=90)
parser.add_argument('--validation', action='store_true')
parser.add_argument('--test-lifecycle', action='store_true')
args = parser.parse_args()
adb = ['adb'] + (['-s', args.serial] if args.serial else [])
package = 'org.remix.sample'
def run(*command, check=True):
    return subprocess.run(adb + list(command), capture_output=True, text=True, check=check, timeout=30)
run('shell', 'am', 'force-stop', package)
run('install', '-r', str(args.apk.resolve()))
run('shell', 'run-as', package, 'mkdir', '-p', 'files')
run('shell', 'run-as', package, 'rm', '-f', 'files/sample.log')
run('shell', 'run-as', package, *(['touch', 'files/validation'] if args.validation else ['rm', '-f', 'files/validation']))
run('shell', 'am', 'start', '-n', package + '/.RemixActivity', '--ei', 'frames', str(args.frames))
start = time.monotonic()
background = False
try:
    while time.monotonic() - start < args.timeout:
        log = run('exec-out', 'run-as', package, 'cat', 'files/sample.log', check=False).stdout
        if 'Android sample exit:' in log:
            print(log)
            if 'Android sample exit: 0' not in log: raise RuntimeError('Sample failed')
            if args.test_lifecycle and not background: raise RuntimeError('Increase --frames to exercise lifecycle')
            break
        if args.test_lifecycle and not background and time.monotonic() - start > 1:
            run('shell', 'input', 'keyevent', 'KEYCODE_HOME')
            time.sleep(2)
            run('shell', 'am', 'start', '-n', package + '/.RemixActivity')
            background = True
        time.sleep(0.25)
    else: raise TimeoutError('Sample did not finish')
finally:
    run('shell', 'am', 'force-stop', package, check=False)
