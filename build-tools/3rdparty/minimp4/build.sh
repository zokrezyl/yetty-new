#!/usr/bin/env bash
# minimp4 3rdparty wrapper. Header-only — single tarball serves every
# target (.noarch marker). Just downloads + stages minimp4.h.
set -euo pipefail
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"
SHELL_NAME="3rdparty-linux-x86_64"
if [ "${USE_NIX:-1}" = "0" ]; then
    exec bash "$(dirname "$0")/_build.sh" "$@"
fi
cd "$(dirname "$0")/../../.."
exec nix develop ".#$SHELL_NAME" --command bash build-tools/3rdparty/minimp4/_build.sh "$@"
