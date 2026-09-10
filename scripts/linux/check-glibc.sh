#!/usr/bin/env bash

set -euo pipefail

if [[ $# -lt 1 || $# -gt 2 ]]; then
  echo "usage: $0 ELF_BINARY [MAX_GLIBC_VERSION]" >&2
  exit 2
fi

binary=$1
baseline=${2:-2.28}
if [[ ! -f "$binary" ]]; then
  echo "ELF binary not found: $binary" >&2
  exit 2
fi

versions=$(readelf --version-info "$binary" | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sed 's/GLIBC_//' | sort -Vu)
newest=$(printf '%s\n' "$versions" | tail -1)
newest_allowed=$(printf '%s\n%s\n' "$baseline" "$newest" | sort -Vu | tail -1)
if [[ "$newest_allowed" != "$baseline" ]]; then
  echo "$binary requires glibc $newest, newer than the $baseline baseline" >&2
  exit 1
fi

echo "$binary requires at most glibc $newest"
