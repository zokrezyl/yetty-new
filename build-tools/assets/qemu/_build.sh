#!/bin/bash
# Dispatches to platforms/$TARGET_PLATFORM.sh. Each platform script is
# responsible for:
#   - setting _CONFIGURE_ARGS (array) with cross/toolchain-specific flags
#   - setting _QEMU_BINARY_NAME (the built binary filename under the build dir)
# Then this script does the common configure + make + strip + package.
#
# Env vars:
#   TARGET_PLATFORM required (see build.sh)
#   VERSION         required — e.g. 0.0.1
#   OUTPUT_DIR      required — where the tarball is written
#   WORK_DIR        optional — default /tmp/yetty-asset-qemu-$TARGET_PLATFORM
#   QEMU_VERSION    optional — default 11.0.0-rc4

set -euo pipefail

: "${TARGET_PLATFORM:?TARGET_PLATFORM is required}"
: "${VERSION:?VERSION is required}"
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WORK_DIR="${WORK_DIR:-/tmp/yetty-asset-qemu-$TARGET_PLATFORM}"
QEMU_VERSION="${QEMU_VERSION:-11.0.0-rc4}"
NCPU="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

PLATFORM_SCRIPT="$SCRIPT_DIR/platforms/${TARGET_PLATFORM}.sh"
[ -f "$PLATFORM_SCRIPT" ] || {
    echo "no platform script for $TARGET_PLATFORM ($PLATFORM_SCRIPT)" >&2
    exit 1
}

QEMU_URL="https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz"
SRC_DIR="$WORK_DIR/qemu-${QEMU_VERSION}"
BUILD_DIR="$WORK_DIR/build-${TARGET_PLATFORM}"
STAGE="$WORK_DIR/stage-${TARGET_PLATFORM}"
TARBALL="$OUTPUT_DIR/qemu-${TARGET_PLATFORM}-${VERSION}.tar.gz"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"
cd "$WORK_DIR"

#-----------------------------------------------------------------------------
# Fetch + extract QEMU source (shared across platform builds)
#-----------------------------------------------------------------------------
if [ ! -f "qemu-${QEMU_VERSION}.tar.xz" ]; then
    echo "==> downloading QEMU ${QEMU_VERSION}"
    curl -fL --retry 3 -o "qemu-${QEMU_VERSION}.tar.xz" "$QEMU_URL"
fi
if [ ! -f "$SRC_DIR/configure" ]; then
    echo "==> extracting QEMU"
    tar xf "qemu-${QEMU_VERSION}.tar.xz"
fi

# Pruned device config (shared with poc/qemu)
DEVCFG_DIR="$SRC_DIR/configs/devices/riscv64-softmmu"
mkdir -p "$DEVCFG_DIR"
cp "$REPO_ROOT/poc/qemu/configs/riscv64-softmmu/default.mak" "$DEVCFG_DIR/default.mak"

#-----------------------------------------------------------------------------
# Common configure flags — mirrored from poc/qemu/build-tools/build-linux-minimal.sh
# Platform script can append to _CONFIGURE_ARGS.
#-----------------------------------------------------------------------------
_CONFIGURE_ARGS=(
    --target-list=riscv64-softmmu
    --without-default-features
    --enable-tcg
    --enable-slirp
    --enable-fdt=internal
    --enable-trace-backends=nop
    --disable-werror
    --disable-docs
    --disable-guest-agent
    --disable-tools
    --disable-qom-cast-debug
    --disable-coroutine-pool
    # virtfs + attr are Linux-only (libattr, attr/xattr.h). Off by
    # default here; linux-*.sh re-enables them.
    --disable-virtfs
    --disable-attr
    --extra-cflags="-Os -ffunction-sections -fdata-sections"
    --extra-cxxflags="-Os -ffunction-sections -fdata-sections"
)
# Non-mach-o linkers support --gc-sections; platform scripts can override
# _EXTRA_LDFLAGS (e.g. darwin uses -dead_strip).
_EXTRA_LDFLAGS="-Wl,--gc-sections"
# Built artifact (may have a platform-specific suffix, e.g. darwin writes
# `qemu-system-riscv64-unsigned` before codesigning).
_QEMU_BINARY_NAME="qemu-system-riscv64"
# Name under which the tarball ships the binary. Defaults to the built
# name; darwin platform scripts normalise it back to `qemu-system-riscv64`.
_QEMU_OUTPUT_NAME=""
_STRIP_BIN="strip"

# shellcheck source=/dev/null
source "$PLATFORM_SCRIPT"

_CONFIGURE_ARGS+=(--extra-ldflags="$_EXTRA_LDFLAGS")

# Force the bundled slirp meson subproject to a static library so libslirp
# is linked into the qemu binary; otherwise qemu ends up with
# RUNPATH=$ORIGIN/subprojects/slirp and a separate libslirp.so.0 to ship.
# QEMU's configure forwards -D<opt>=<val> to meson.
_CONFIGURE_ARGS+=(-Dslirp:default_library=static)

#-----------------------------------------------------------------------------
# Configure + build
#-----------------------------------------------------------------------------
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "==> configuring QEMU for $TARGET_PLATFORM"
"$SRC_DIR/configure" "${_CONFIGURE_ARGS[@]}"

echo "==> building (-j${NCPU})"
make -j"$NCPU"

BUILT="$BUILD_DIR/$_QEMU_BINARY_NAME"
[ -f "$BUILT" ] || { echo "missing binary: $BUILT" >&2; exit 1; }

# Strip if the platform toolchain provides a compatible strip
if command -v "$_STRIP_BIN" >/dev/null 2>&1; then
    "$_STRIP_BIN" "$BUILT" || true
fi

#-----------------------------------------------------------------------------
# Stage + package
#-----------------------------------------------------------------------------
rm -rf "$STAGE"
mkdir -p "$STAGE"

OUT_NAME="${_QEMU_OUTPUT_NAME:-$_QEMU_BINARY_NAME}"
cp "$BUILT" "$STAGE/$OUT_NAME"

echo "==> packaging -> $TARBALL"
tar -C "$STAGE" -czf "$TARBALL" .

echo ""
echo "QEMU asset ready ($TARGET_PLATFORM):"
ls -lh "$TARBALL"
tar -tzf "$TARBALL"
