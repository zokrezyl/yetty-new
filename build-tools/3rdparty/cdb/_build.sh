#!/bin/bash
# Build yetty-ymsdf-gen host tool and generate MSDF CDB font databases from
# the DejaVuSansMNerdFontMono TTFs in assets/fonts/. Produces cdb-${VERSION}.tar.gz.
#
# Env vars:
#   VERSION      required — e.g. 0.0.1; used in output filename
#   OUTPUT_DIR   required — where to place the tarball
#   WORK_DIR     optional — intermediate build tree (default: /tmp/yetty-asset-cdb)
#
# Run via the wrapper build.sh (which handles the nix develop).

set -euo pipefail

# Version is read from ./version file — single source of truth (matches
# the lib-<name>-<version> tag pushed via build-tools/push-3rdparty-tag.sh).
VERSION_FILE="$(dirname "$0")/version"
[ -f "$VERSION_FILE" ] || { echo "missing $VERSION_FILE" >&2; exit 1; }
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[ -n "$VERSION" ] || { echo "$VERSION_FILE is empty" >&2; exit 1; }
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WORK_DIR="${WORK_DIR:-/tmp/yetty-asset-cdb}"

NCPU="$(nproc 2>/dev/null || echo 4)"

FONT_DIR="$REPO_ROOT/assets/fonts"
TTF_FILES=(
    "$FONT_DIR/DejaVuSansMNerdFontMono-Regular.ttf"
    "$FONT_DIR/DejaVuSansMNerdFontMono-Bold.ttf"
    "$FONT_DIR/DejaVuSansMNerdFontMono-Oblique.ttf"
    "$FONT_DIR/DejaVuSansMNerdFontMono-BoldOblique.ttf"
)

for f in "${TTF_FILES[@]}"; do
    [ -f "$f" ] || { echo "missing TTF: $f" >&2; exit 1; }
done

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"

BUILD_DIR="$WORK_DIR/build"
STAGE="$WORK_DIR/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

echo "==> configuring host tools"
cmake -S "$REPO_ROOT/build-tools/cmake/host-tools" -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=Release

echo "==> building yetty-ymsdf-gen (-j${NCPU})"
cmake --build "$BUILD_DIR" --target yetty-ymsdf-gen --parallel "$NCPU"

YMSDF_GEN="$BUILD_DIR/yetty-ymsdf-gen"
[ -x "$YMSDF_GEN" ] || { echo "missing binary: $YMSDF_GEN" >&2; exit 1; }

RAW_DIR="$WORK_DIR/cdb-raw"
rm -rf "$RAW_DIR"
mkdir -p "$RAW_DIR"

for ttf in "${TTF_FILES[@]}"; do
    echo "==> generating CDB for $(basename "$ttf")"
    "$YMSDF_GEN" --all "$ttf" "$RAW_DIR"
done

# Sanity check — expect one .cdb per TTF
CDB_COUNT="$(find "$RAW_DIR" -maxdepth 1 -name '*.cdb' | wc -l)"
if [ "$CDB_COUNT" -ne "${#TTF_FILES[@]}" ]; then
    echo "expected ${#TTF_FILES[@]} cdb files, got $CDB_COUNT" >&2
    ls -la "$RAW_DIR" >&2
    exit 1
fi

# Brotli-compress each CDB into the stage. The main build's incbin
# pipeline embeds these verbatim and decompresses at runtime (same
# scheme used by build-tools/cmake/incbin.cmake with COMPRESS=TRUE).
# q11 = max compression. CI runs once per release; runtime decompresses
# in-binary at startup, so producer-side cost is amortised forever.
: "${BROTLI_QUALITY:=11}"
for cdb in "$RAW_DIR"/*.cdb; do
    name="$(basename "$cdb")"
    in_size="$(stat -c%s "$cdb" 2>/dev/null || stat -f%z "$cdb")"
    echo "==> brotli ${name} (quality $BROTLI_QUALITY)"
    brotli -q "$BROTLI_QUALITY" -f -o "$STAGE/${name}.br" "$cdb"
    out_size="$(stat -c%s "$STAGE/${name}.br" 2>/dev/null || stat -f%z "$STAGE/${name}.br")"
    printf "    %-50s  %10d -> %10d bytes\n" "$name" "$in_size" "$out_size"
done

TARBALL="$OUTPUT_DIR/cdb-${VERSION}.tar.gz"
echo "==> packaging -> $TARBALL"
tar -C "$STAGE" -czf "$TARBALL" .

echo ""
echo "CDB asset ready:"
ls -lh "$TARBALL"
tar -tzf "$TARBALL"
