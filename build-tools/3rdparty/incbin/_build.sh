#!/bin/bash
# Builds the noarch incbin tarball — fetches incbin.h + incbin.c from
# graphitemaster/incbin@<sha> and packages them. No compilation: yetty's
# main build uses incbin.h via inline assembly (GCC/Clang) at link time,
# and on MSVC a separate host build of incbin_tool compiles incbin.c
# (handled at consumer side, not by this script).
#
# Required env:
#   OUTPUT_DIR  where the tarball is written
#
# Version: this directory's `version` file holds a commit SHA (incbin
# doesn't tag releases).
#
# Output tarball layout (consumed by build-tools/cmake/incbin.cmake):
#   include/incbin.h
#   src/incbin.c   (optional, used only by the MSVC code path)

set -Eeuo pipefail
trap 'rc=$?; echo "FAILED: rc=$rc line=$LINENO source=${BASH_SOURCE[0]} cmd: $BASH_COMMAND" >&2' ERR

: "${OUTPUT_DIR:?OUTPUT_DIR is required}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
VERSION_FILE="$SCRIPT_DIR/version"
[ -f "$VERSION_FILE" ] || { echo "missing $VERSION_FILE" >&2; exit 1; }
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[ -n "$VERSION" ] || { echo "$VERSION_FILE is empty" >&2; exit 1; }

WORK_DIR="${WORK_DIR:-/tmp/yetty-3rdparty-incbin}"
CACHE_DIR="${CACHE_DIR:-$HOME/.cache/yetty-3rdparty}"

INCBIN_URL="https://github.com/graphitemaster/incbin/archive/${VERSION}.tar.gz"
INCBIN_TARBALL="$CACHE_DIR/incbin-${VERSION}.tar.gz"
SRC_DIR="$WORK_DIR/incbin-${VERSION}"
STAGE="$WORK_DIR/stage"
TARBALL="$OUTPUT_DIR/incbin-${VERSION}.tar.gz"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR" "$CACHE_DIR"

if [ ! -f "$INCBIN_TARBALL" ]; then
    _part="$INCBIN_TARBALL.part.$$"
    (
        if command -v flock >/dev/null 2>&1; then flock -x 9; fi
        if [ ! -f "$INCBIN_TARBALL" ]; then
            echo "==> downloading incbin @${VERSION:0:8}"
            curl -fL --retry 8 --retry-delay 5 --retry-all-errors \
                -o "$_part" "$INCBIN_URL"
            mv "$_part" "$INCBIN_TARBALL"
        fi
    ) 9>"$CACHE_DIR/.incbin-download.lock"
    rm -f "$_part"
fi

if [ ! -d "$SRC_DIR" ]; then
    echo "==> extracting -> $SRC_DIR"
    mkdir -p "$WORK_DIR/.extract-$$"
    tar -C "$WORK_DIR/.extract-$$" -xzf "$INCBIN_TARBALL"
    mv "$WORK_DIR/.extract-$$/incbin-${VERSION}" "$SRC_DIR"
    rmdir "$WORK_DIR/.extract-$$"
fi
rm -rf "$STAGE"
mkdir -p "$STAGE/include" "$STAGE/src"

#-----------------------------------------------------------------------------
# Stage incbin.h + incbin.c
#-----------------------------------------------------------------------------
[ -f "$SRC_DIR/incbin.h" ] || { echo "missing incbin.h in source" >&2; exit 1; }
cp "$SRC_DIR/incbin.h" "$STAGE/include/"
if [ -f "$SRC_DIR/incbin.c" ]; then
    cp "$SRC_DIR/incbin.c" "$STAGE/src/"
fi

echo "==> packaging -> $TARBALL"
tar -C "$STAGE" -czf "$TARBALL" .

echo ""
echo "incbin @${VERSION:0:8} (noarch) ready:"
ls -lh "$TARBALL"
tar -tzf "$TARBALL"
