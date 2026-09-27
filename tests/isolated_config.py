"""Run a test with its own per-game configuration, never the developer's profiles."""
import os
from pathlib import Path
import subprocess
import sys
import signal
import tempfile

# Keep temporary per-game settings inside the workspace build area.
root = Path(__file__).resolve().parents[1] / "_work/build/profile-test-runs"
root.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory(prefix="config-", dir=root) as directory:
    # Optional suite cache: profiles stay isolated while GPU regression runs
    # reuse the same device/driver cache, as successive application launches do.
    shared_cache = os.environ.get("REMIX_TEST_SHARED_PIPELINE_CACHE")
    if shared_cache:
        cache = Path(shared_cache).resolve()
        cache.mkdir(parents=True, exist_ok=True)
        (Path(directory) / "cache").symlink_to(cache, target_is_directory=True)
    env = dict(os.environ, REMIX_CONFIG_DIR=directory)
    command = [directory if arg == "@CONFIG_DIR@" else arg for arg in sys.argv[1:]]
    process = subprocess.Popen(command, env=env)

    def stop(signum, _frame):
        process.terminate()
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait()
        raise SystemExit(128 + signum)

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    sys.exit(process.wait())
