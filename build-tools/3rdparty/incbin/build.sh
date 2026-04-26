#!/usr/bin/env bash
# incbin 3rdparty wrapper. Noarch — just ships incbin.h (the header that
# defines the INCBIN macro for inline-asm-based binary embedding) plus
# incbin.c (used to build incbin_tool on MSVC). No per-target build.
#
# Required env:
#   OUTPUT_DIR  where the tarball is written

set -euo pipefail
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"

# Single ubuntu-friendly nix shell is enough — no compilation, just curl + tar.
SHELL_NAME="3rdparty-linux-x86_64"

if [ "${USE_NIX:-1}" = "0" ]; then
    exec bash "$(dirname "$0")/_build.sh" "$@"
fi

cd "$(dirname "$0")/../../.."
exec nix develop ".#$SHELL_NAME" --command bash build-tools/3rdparty/incbin/_build.sh "$@"
