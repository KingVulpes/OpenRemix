#!/usr/bin/env bash
set -euo pipefail
if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 BUILD_DIRECTORY [SDL_DEPENDENCY_PREFIX]" >&2
  exit 2
fi
root=$(cd "$(dirname "$0")/../.." && pwd)
args=("$1" "$root" -Dbuildtype=release -Dwerror=true)
if [[ $# == 2 ]]; then args+=("-Ddeps_root=$(realpath "$2")"); fi
if [[ -f "$1/meson-private/coredata.dat" ]]; then args+=(--reconfigure); fi
CC=clang CXX=clang++ meson setup "${args[@]}"
meson compile -C "$1"
meson test -C "$1" --print-errorlogs
