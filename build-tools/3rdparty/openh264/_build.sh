#!/bin/bash
# Builds openh264 (cisco/openh264) for $TARGET_PLATFORM and packages it as
# a tarball under $OUTPUT_DIR. Mirrors the per-platform make invocations in
# build-tools/cmake/openh264.cmake — but produces an installable bundle
# (lib/ + include/) that 3rdparty-fetch.cmake can drop into the build tree.
#
# Required env:
#   TARGET_PLATFORM   linux-x86_64 | linux-aarch64 |
#                     macos-arm64 | macos-x86_64 |
#                     android-arm64-v8a | android-x86_64 |
#                     ios-arm64 | ios-x86_64 |
#                     webasm | windows-x86_64
#   VERSION           e.g. 0.0.1 — release tarball version (NOT openh264 ver)
#   OUTPUT_DIR        where the tarball is written
#
# Optional env:
#   WORK_DIR          default /tmp/yetty-3rdparty-openh264-$TARGET_PLATFORM
#   CACHE_DIR         default $HOME/.cache/yetty-3rdparty
#                     holds the openh264 source tarball so multi-target
#                     builds share a single download
#   OPENH264_VERSION  default 2.4.1 (must match versions.txt)
#   ANDROID_API       default 26 (matches yetty Android build)
#   IOS_MIN           default 15.0
#   CROSS_PREFIX      default aarch64-unknown-linux-gnu- (linux-aarch64)

set -Eeuo pipefail
trap 'rc=$?; echo "FAILED: rc=$rc line=$LINENO source=${BASH_SOURCE[0]} cmd: $BASH_COMMAND" >&2' ERR

: "${TARGET_PLATFORM:?TARGET_PLATFORM is required}"
: "${VERSION:?VERSION is required}"
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WORK_DIR="${WORK_DIR:-/tmp/yetty-3rdparty-openh264-$TARGET_PLATFORM}"
CACHE_DIR="${CACHE_DIR:-$HOME/.cache/yetty-3rdparty}"
OPENH264_VERSION="${OPENH264_VERSION:-2.4.1}"
NCPU="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

OPENH264_URL="https://github.com/cisco/openh264/archive/refs/tags/v${OPENH264_VERSION}.tar.gz"
OPENH264_TARBALL="$CACHE_DIR/openh264-${OPENH264_VERSION}.tar.gz"
SRC_DIR="$WORK_DIR/openh264-${OPENH264_VERSION}-${TARGET_PLATFORM}"
INSTALL_DIR="$WORK_DIR/install-${TARGET_PLATFORM}"
STAGE="$WORK_DIR/stage-${TARGET_PLATFORM}"
TARBALL="$OUTPUT_DIR/openh264-${TARGET_PLATFORM}-${VERSION}.tar.gz"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR" "$CACHE_DIR"

#-----------------------------------------------------------------------------
# Fetch (shared across targets) — flock so parallel target builds share
# a single download. Per-PID .part avoids clobbering on hosts without flock.
#-----------------------------------------------------------------------------
if [ ! -f "$OPENH264_TARBALL" ]; then
    _part="$OPENH264_TARBALL.part.$$"
    (
        if command -v flock >/dev/null 2>&1; then
            flock -x 9
        fi
        if [ ! -f "$OPENH264_TARBALL" ]; then
            echo "==> downloading openh264 ${OPENH264_VERSION}"
            curl -fL --retry 3 -o "$_part" "$OPENH264_URL"
            mv "$_part" "$OPENH264_TARBALL"
        fi
    ) 9>"$CACHE_DIR/.openh264-download.lock"
    rm -f "$_part"
else
    echo "==> using cached openh264 source: $OPENH264_TARBALL"
fi

#-----------------------------------------------------------------------------
# Extract — openh264 builds in-source, so we keep one extraction per target
# (multiple TARGET_PLATFORM builds in the same WORK_DIR don't collide).
#-----------------------------------------------------------------------------
rm -rf "$SRC_DIR" "$INSTALL_DIR" "$STAGE"
mkdir -p "$INSTALL_DIR" "$STAGE"
echo "==> extracting -> $SRC_DIR"
mkdir -p "$WORK_DIR/.extract-${TARGET_PLATFORM}"
tar -C "$WORK_DIR/.extract-${TARGET_PLATFORM}" -xzf "$OPENH264_TARBALL"
mv "$WORK_DIR/.extract-${TARGET_PLATFORM}/openh264-${OPENH264_VERSION}" "$SRC_DIR"
rmdir "$WORK_DIR/.extract-${TARGET_PLATFORM}"

#-----------------------------------------------------------------------------
# Per-platform make args. openh264's top-level Makefile understands OS=,
# ARCH=, NDKROOT=, etc.; we pick the right ones here.
#-----------------------------------------------------------------------------
COMMON_ARGS=(
    BUILDTYPE=Release
    ENABLE_SHARED=No
    "PREFIX=$INSTALL_DIR"
)
EXTRA_ARGS=()
MAKE_WRAPPER=""   # "" for plain make, "emmake" for webasm

case "$TARGET_PLATFORM" in

linux-x86_64)
    EXTRA_ARGS=(ARCH=x86_64)
    ;;

linux-aarch64)
    : "${CROSS_PREFIX:=aarch64-unknown-linux-gnu-}"
    EXTRA_ARGS=(
        ARCH=arm64
        "CC=${CROSS_PREFIX}gcc"
        "CXX=${CROSS_PREFIX}g++"
        "AR=${CROSS_PREFIX}ar"
        "AS=${CROSS_PREFIX}as"
        "STRIP=${CROSS_PREFIX}strip"
    )
    ;;

macos-x86_64)
    EXTRA_ARGS=(OS=darwin ARCH=x86_64)
    ;;

macos-arm64)
    EXTRA_ARGS=(OS=darwin ARCH=arm64)
    ;;

android-arm64-v8a|android-x86_64)
    : "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME not set — source the .#3rdparty-${TARGET_PLATFORM} shell}"
    : "${ANDROID_API:=26}"
    case "$TARGET_PLATFORM" in
        android-arm64-v8a) _OH264_ARCH=arm64 ;;
        android-x86_64)    _OH264_ARCH=x86_64 ;;
    esac
    EXTRA_ARGS=(
        OS=android
        "ARCH=${_OH264_ARCH}"
        "NDKROOT=${ANDROID_NDK_HOME}"
        "TARGET=android-${ANDROID_API}"
        "NDKLEVEL=${ANDROID_API}"
    )
    ;;

ios-arm64|ios-x86_64)
    : "${IOS_MIN:=15.0}"
    case "$TARGET_PLATFORM" in
        ios-arm64)  _IOS_SDK="iphoneos";        _OH264_ARCH=arm64  ;;
        ios-x86_64) _IOS_SDK="iphonesimulator"; _OH264_ARCH=x86_64 ;;
    esac
    EXTRA_ARGS=(
        OS=ios
        "ARCH=${_OH264_ARCH}"
        "SDK_MIN=${IOS_MIN}"
        "CC=xcrun -sdk ${_IOS_SDK} clang"
        "CXX=xcrun -sdk ${_IOS_SDK} clang++"
        "AR=xcrun -sdk ${_IOS_SDK} ar"
    )
    ;;

webasm)
    command -v emmake >/dev/null 2>&1 || {
        echo "error: emmake not found — source the .#3rdparty-webasm shell" >&2
        exit 1
    }
    # Match the cmake recipe: ARCH= (empty) prevents -m64 flags; -D__APPLE__
    # forces openh264 to use pthread_cond_t instead of sem_timedwait (not in
    # emscripten); USE_ASM=No because we have no NASM target for wasm.
    EXTRA_ARGS=(
        USE_ASM=No
        ARCH=
        "CFLAGS=-pthread -D__APPLE__"
        "CXXFLAGS=-pthread -D__APPLE__"
        "LDFLAGS=-pthread"
    )
    MAKE_WRAPPER="emmake"
    ;;

windows-x86_64)
    # Cisco's Makefile supports OS=msvc — invoked through GNU make
    # (chocolatey: `choco install make`) under a vcvarsall'd shell.
    EXTRA_ARGS=(OS=msvc ARCH=x86_64)
    ;;

*)
    echo "unknown TARGET_PLATFORM: $TARGET_PLATFORM" >&2
    exit 1
    ;;
esac

#-----------------------------------------------------------------------------
# Build + install. openh264 builds in-source, so cd into SRC_DIR and clear
# stamps from any prior (cancelled) run — `make clean` before each phase.
#-----------------------------------------------------------------------------
echo "==> building openh264 for $TARGET_PLATFORM (-j${NCPU})"
echo "    args: ${COMMON_ARGS[*]} ${EXTRA_ARGS[*]}"

cd "$SRC_DIR"

if [ -n "$MAKE_WRAPPER" ]; then
    "$MAKE_WRAPPER" make -j"$NCPU" "${COMMON_ARGS[@]}" "${EXTRA_ARGS[@]}" libraries
    "$MAKE_WRAPPER" make            "${COMMON_ARGS[@]}" "${EXTRA_ARGS[@]}" install-static
else
    make -j"$NCPU" "${COMMON_ARGS[@]}" "${EXTRA_ARGS[@]}" libraries
    make            "${COMMON_ARGS[@]}" "${EXTRA_ARGS[@]}" install-static
fi

#-----------------------------------------------------------------------------
# Verify install layout
#-----------------------------------------------------------------------------
case "$TARGET_PLATFORM" in
    windows-x86_64) _LIB="$INSTALL_DIR/lib/openh264.lib"     ;;
    *)              _LIB="$INSTALL_DIR/lib/libopenh264.a"    ;;
esac

if [ ! -f "$_LIB" ]; then
    echo "missing library: $_LIB" >&2
    echo "install tree:" >&2
    find "$INSTALL_DIR" -maxdepth 4 -print >&2 || true
    exit 1
fi

if [ ! -d "$INSTALL_DIR/include/wels" ]; then
    echo "missing headers: $INSTALL_DIR/include/wels/" >&2
    exit 1
fi

#-----------------------------------------------------------------------------
# Stage + package — keep the install layout (lib/, include/) so the
# 3rdparty-fetch consumer can hardcode the same paths Dawn/Dav1d use.
#-----------------------------------------------------------------------------
cp -a "$INSTALL_DIR/lib"     "$STAGE/"
cp -a "$INSTALL_DIR/include" "$STAGE/"

echo "==> packaging -> $TARBALL"
tar -C "$STAGE" -czf "$TARBALL" .

echo ""
echo "openh264 $OPENH264_VERSION ($TARGET_PLATFORM) ready:"
ls -lh "$TARBALL"
ENTRIES="$(tar -tzf "$TARBALL" | wc -l)"
echo "contents (first 20 of $ENTRIES):"
tar -tzf "$TARBALL" | sed -n '1,20p'
