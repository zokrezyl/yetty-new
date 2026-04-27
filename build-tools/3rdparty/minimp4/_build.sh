#!/bin/bash
# Builds the noarch minimp4 tarball — fetches minimp4.h from
# lieff/minimp4@<sha>. Header-only, no compile.
#
# Output tarball layout (consumed by build-tools/cmake/minimp4.cmake):
#   include/minimp4.h

set -Eeuo pipefail
trap 'rc=$?; echo "FAILED: rc=$rc line=$LINENO source=${BASH_SOURCE[0]} cmd: $BASH_COMMAND" >&2' ERR

: "${OUTPUT_DIR:?OUTPUT_DIR is required}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION="$(tr -d '[:space:]' < "$SCRIPT_DIR/version")"

WORK_DIR="${WORK_DIR:-/tmp/yetty-3rdparty-minimp4}"
CACHE_DIR="${CACHE_DIR:-$HOME/.cache/yetty-3rdparty}"

URL="https://github.com/lieff/minimp4/archive/${VERSION}.tar.gz"
TARBALL_CACHE="$CACHE_DIR/minimp4-${VERSION}.tar.gz"
SRC_DIR="$WORK_DIR/minimp4-${VERSION}"
STAGE="$WORK_DIR/stage"
TARBALL="$OUTPUT_DIR/minimp4-${VERSION}.tar.gz"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR" "$CACHE_DIR"

if [ ! -f "$TARBALL_CACHE" ]; then
    _part="$TARBALL_CACHE.part.$$"
    (
        if command -v flock >/dev/null 2>&1; then flock -x 9; fi
        if [ ! -f "$TARBALL_CACHE" ]; then
            echo "==> downloading minimp4 @${VERSION:0:8}"
            curl -fL --retry 8 --retry-delay 5 --retry-all-errors -o "$_part" "$URL"
            mv "$_part" "$TARBALL_CACHE"
        fi
    ) 9>"$CACHE_DIR/.minimp4-download.lock"
    rm -f "$_part"
fi

if [ ! -d "$SRC_DIR" ]; then
    echo "==> extracting -> $SRC_DIR"
    mkdir -p "$WORK_DIR/.extract-$$"
    tar -C "$WORK_DIR/.extract-$$" -xzf "$TARBALL_CACHE"
    mv "$WORK_DIR/.extract-$$/minimp4-${VERSION}" "$SRC_DIR"
    rmdir "$WORK_DIR/.extract-$$"
fi
rm -rf "$STAGE"
mkdir -p "$STAGE/include"

[ -f "$SRC_DIR/minimp4.h" ] || { echo "missing minimp4.h" >&2; exit 1; }
cp "$SRC_DIR/minimp4.h" "$STAGE/include/"

tar -C "$STAGE" -czf "$TARBALL" .
echo "minimp4 @${VERSION:0:8} (noarch) ready:"
ls -lh "$TARBALL"
tar -tzf "$TARBALL"
