#!/bin/bash
# Build OpenSBI RISC-V firmware and produce opensbi-${VERSION}.tar.gz
#
# Env vars:
#   VERSION      required — e.g. 0.0.1; used in output filename
#   OUTPUT_DIR   required — where to place the tarball
#   WORK_DIR     optional — intermediate build tree (default: /tmp/yetty-asset-opensbi)
#   OPENSBI_VERSION   optional — upstream tag (default: 1.4)
#   CROSS_COMPILE     optional — toolchain prefix (default: riscv64-linux-gnu-)
#
# Hermetic Linux build: needs only make, wget/curl, tar, and the RISC-V
# cross-toolchain. No host-arch-specific output — the firmware is RISC-V
# regardless of where this script runs.

set -euo pipefail

: "${VERSION:?VERSION is required}"
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"
WORK_DIR="${WORK_DIR:-/tmp/yetty-asset-opensbi}"
OPENSBI_VERSION="${OPENSBI_VERSION:-1.4}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-linux-gnu-}"

NCPU="$(nproc 2>/dev/null || echo 4)"

SRC_URL="https://github.com/riscv-software-src/opensbi/archive/refs/tags/v${OPENSBI_VERSION}.tar.gz"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"
cd "$WORK_DIR"

if [ ! -f "opensbi-${OPENSBI_VERSION}.tar.gz" ]; then
    echo "==> downloading OpenSBI ${OPENSBI_VERSION}"
    curl -fL --retry 3 -o "opensbi-${OPENSBI_VERSION}.tar.gz" "$SRC_URL"
fi

if [ ! -d "opensbi-${OPENSBI_VERSION}" ]; then
    echo "==> extracting"
    tar xf "opensbi-${OPENSBI_VERSION}.tar.gz"
fi

cd "opensbi-${OPENSBI_VERSION}"

echo "==> building (CROSS_COMPILE=${CROSS_COMPILE}, -j${NCPU})"
# -std=gnu11: OpenSBI 1.4 typedefs `bool`, which GCC 15 (default C23) rejects.
# Pass via platform-genflags-y so we don't clobber the Makefile's CFLAGS.
make -j"$NCPU" \
    CROSS_COMPILE="$CROSS_COMPILE" \
    PLATFORM=generic \
    FW_JUMP=y \
    platform-genflags-y="-std=gnu11"

FW_JUMP="build/platform/generic/firmware/fw_jump.elf"
FW_DYNAMIC="build/platform/generic/firmware/fw_dynamic.bin"
[ -f "$FW_JUMP" ]    || { echo "missing $FW_JUMP"    >&2; exit 1; }
[ -f "$FW_DYNAMIC" ] || { echo "missing $FW_DYNAMIC" >&2; exit 1; }

STAGE="$WORK_DIR/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$FW_JUMP"    "$STAGE/opensbi-fw_jump.elf"
cp "$FW_DYNAMIC" "$STAGE/opensbi-fw_dynamic.bin"

TARBALL="$OUTPUT_DIR/opensbi-${VERSION}.tar.gz"
echo "==> packaging -> $TARBALL"
tar -C "$STAGE" -czf "$TARBALL" .

echo ""
echo "OpenSBI asset ready:"
ls -lh "$TARBALL"
tar -tzf "$TARBALL"
