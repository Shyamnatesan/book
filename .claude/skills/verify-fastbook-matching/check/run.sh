#!/usr/bin/env bash
# Build and run the exhaustive FastBook check.
# Usage: run.sh [include_dir] [-- extra args, e.g. --state-limit N]
#   include_dir defaults to the repo's include/. Pass a copy to check a mutant.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../../../.." && pwd)"
inc="${1:-$repo/include}"
[[ $# -gt 0 ]] && shift
[[ "${1:-}" == "--" ]] && shift
out="${TMPDIR:-/tmp}/verify-fastbook-matching"
mkdir -p "$out"
bin="$out/book_exhaustive.$$"
# No -DNDEBUG: the library's asserts are part of the check.
g++ -std=c++23 -O1 -g -fno-omit-frame-pointer \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -I"$inc" "$here/book_exhaustive.cpp" -o "$bin"
trap 'rm -f "$bin"' EXIT
"$bin" "$@"
