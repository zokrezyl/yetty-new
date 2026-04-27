#!/bin/bash
# Builds libmagic (file 5.x) for $TARGET_PLATFORM via the upstream
# autotools configure script. Replaces the per-yetty-configure
# ExternalProject_Add(libmagic_ext …) in build-tools/cmake/Libmagic.cmake.
#
# Required env:
#   TARGET_PLATFORM   linux-x86_64 | linux-aarch64 |
#                     macos-arm64 | macos-x86_64 |
#                     android-arm64-v8a | android-x86_64 |
#                     ios-arm64 | ios-x86_64 | tvos-x86_64 |
#                     webasm | windows-x86_64
#   OUTPUT_DIR        where the tarball is written
#
# Version is read from this directory's `version` file. Distribution
# tarball is fetched from astron.com (the maintainer's mirror — same
# URL the upstream README points at).
#
# Optional env:
#   WORK_DIR          default /tmp/yetty-3rdparty-libmagic-$TARGET_PLATFORM
#   CACHE_DIR         default $HOME/.cache/yetty-3rdparty
#   ANDROID_API       default 26
#   IOS_MIN           default 15.0
#   CROSS_PREFIX      default aarch64-unknown-linux-gnu-
#
# Output tarball layout (consumed by build-tools/cmake/Libmagic.cmake):
#   lib/libmagic.a (or magic.lib on windows native MSVC)
#   include/magic.h
#   share/misc/magic.mgc      (compiled magic database — yetty embeds this at runtime)
#
# Cross-compile note:
# file's `mkmagic` (the tool that compiles src/Magdir/* → magic.mgc) must
# be a NATIVE binary even when cross-compiling. We do a small native
# pre-build to produce mkmagic, then cross-build the actual library
# pointing at it via `FILE_COMPILE`. Same trick used by mainstream
# distros' cross packages.

set -Eeuo pipefail
trap 'rc=$?; echo "FAILED: rc=$rc line=$LINENO source=${BASH_SOURCE[0]} cmd: $BASH_COMMAND" >&2' ERR

: "${TARGET_PLATFORM:?TARGET_PLATFORM is required}"
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
VERSION_FILE="$SCRIPT_DIR/version"
[ -f "$VERSION_FILE" ] || { echo "missing $VERSION_FILE" >&2; exit 1; }
VERSION="$(tr -d '[:space:]' < "$VERSION_FILE")"
[ -n "$VERSION" ] || { echo "$VERSION_FILE is empty" >&2; exit 1; }

WORK_DIR="${WORK_DIR:-/tmp/yetty-3rdparty-libmagic-$TARGET_PLATFORM}"
CACHE_DIR="${CACHE_DIR:-$HOME/.cache/yetty-3rdparty}"
NCPU="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

LIBMAGIC_URL="https://astron.com/pub/file/file-${VERSION}.tar.gz"
LIBMAGIC_TARBALL="$CACHE_DIR/file-${VERSION}.tar.gz"
SRC_BASE="$WORK_DIR/file-${VERSION}-src"
NATIVE_DIR="$WORK_DIR/native-build"   # for mkmagic bootstrap (cross only)
SRC_DIR="$WORK_DIR/file-${VERSION}-${TARGET_PLATFORM}"
INSTALL_DIR="$WORK_DIR/install-${TARGET_PLATFORM}"
STAGE="$WORK_DIR/stage-${TARGET_PLATFORM}"
TARBALL="$OUTPUT_DIR/libmagic-${TARGET_PLATFORM}-${VERSION}.tar.gz"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR" "$CACHE_DIR"

#-----------------------------------------------------------------------------
# Fetch
#-----------------------------------------------------------------------------
if [ ! -f "$LIBMAGIC_TARBALL" ]; then
    _part="$LIBMAGIC_TARBALL.part.$$"
    (
        if command -v flock >/dev/null 2>&1; then flock -x 9; fi
        if [ ! -f "$LIBMAGIC_TARBALL" ]; then
            echo "==> downloading file-${VERSION}"
            curl -fL --retry 8 --retry-delay 5 --retry-all-errors \
                -o "$_part" "$LIBMAGIC_URL"
            mv "$_part" "$LIBMAGIC_TARBALL"
        fi
    ) 9>"$CACHE_DIR/.libmagic-download.lock"
    rm -f "$_part"
fi

# Per-target source extraction (autotools is in-source, so each target
# needs its own copy). The shared SRC_BASE is just a marker for the
# initial untar — we then rsync into per-target dirs.
if [ ! -d "$SRC_BASE" ]; then
    echo "==> extracting source -> $SRC_BASE"
    mkdir -p "$WORK_DIR/.extract-$$"
    tar -C "$WORK_DIR/.extract-$$" -xzf "$LIBMAGIC_TARBALL"
    mv "$WORK_DIR/.extract-$$/file-${VERSION}" "$SRC_BASE"
    rmdir "$WORK_DIR/.extract-$$"
fi
rm -rf "$SRC_DIR" "$INSTALL_DIR" "$STAGE"
cp -a "$SRC_BASE" "$SRC_DIR"
mkdir -p "$INSTALL_DIR" "$STAGE"

# Touch auto-generated files so timestamps don't trigger autoreconf
# (same workaround as the original cmake).
find "$SRC_DIR" \
    \( -name "aclocal.m4" -o -name "configure" -o -name "Makefile.in" -o -name "config.h.in" \) \
    -exec touch {} \;

#-----------------------------------------------------------------------------
# Per-platform configure args.
#-----------------------------------------------------------------------------
COMMON_FLAGS=(
    --prefix="$INSTALL_DIR"
    --disable-shared
    --enable-static
    --disable-libseccomp
    --disable-bzlib
    --disable-xzlib
    --disable-zstdlib
    --disable-lzlib
    --disable-zlib
    --disable-maintainer-mode
)

CONFIGURE_ENV=()       # extra env vars for ./configure
EXTRA_CONFIGURE=()     # extra ./configure args (e.g. --host=)
NEEDS_NATIVE_BOOTSTRAP=0    # 1 → build mkmagic natively first
MAKE_PREFIX=""         # "emmake " for webasm

# Default CC for the target. On macOS native, autotools's default
# compiler discovery picks system clang, which is what we want.
CC=cc

case "$TARGET_PLATFORM" in

linux-x86_64)
    CC=gcc
    ;;

linux-aarch64)
    : "${CROSS_PREFIX:=aarch64-unknown-linux-gnu-}"
    CC="${CROSS_PREFIX}gcc"
    EXTRA_CONFIGURE+=("--host=aarch64-linux-gnu")
    NEEDS_NATIVE_BOOTSTRAP=1
    ;;

macos-x86_64)
    CC="clang -arch x86_64"
    ;;

macos-arm64)
    CC="clang -arch arm64"
    ;;

ios-arm64|ios-x86_64|tvos-x86_64)
    # nix-on-macOS xcrun trap.
    unset DEVELOPER_DIR MACOSX_DEPLOYMENT_TARGET SDKROOT NIX_APPLE_SDK_VERSION
    export PATH="/usr/bin:$PATH"
    : "${IOS_MIN:=15.0}"
    : "${TVOS_MIN:=17.0}"
    case "$TARGET_PLATFORM" in
        ios-arm64)
            _IOS_SDK="iphoneos"; _IOS_ARCH="arm64"
            EXTRA_CONFIGURE+=("--host=arm-apple-darwin")
            _MIN_FLAG="-miphoneos-version-min=${IOS_MIN}"
            ;;
        ios-x86_64)
            _IOS_SDK="iphonesimulator"; _IOS_ARCH="x86_64"
            EXTRA_CONFIGURE+=("--host=x86_64-apple-darwin")
            _MIN_FLAG="-mios-simulator-version-min=${IOS_MIN}"
            ;;
        tvos-x86_64)
            _IOS_SDK="appletvsimulator"; _IOS_ARCH="x86_64"
            EXTRA_CONFIGURE+=("--host=x86_64-apple-darwin")
            _MIN_FLAG="-mtvos-simulator-version-min=${TVOS_MIN}"
            ;;
    esac
    _IOS_SYSROOT="$(/usr/bin/xcrun --sdk "$_IOS_SDK" --show-sdk-path)"
    [ -d "$_IOS_SYSROOT" ] || { echo "missing SDK: $_IOS_SYSROOT" >&2; exit 1; }
    CC="/usr/bin/clang -arch $_IOS_ARCH -isysroot $_IOS_SYSROOT $_MIN_FLAG"
    NEEDS_NATIVE_BOOTSTRAP=1
    ;;

android-arm64-v8a|android-x86_64)
    : "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME not set — source the .#3rdparty-${TARGET_PLATFORM} shell}"
    : "${ANDROID_API:=26}"
    case "$TARGET_PLATFORM" in
        android-arm64-v8a)
            _NDK_TRIPLE="aarch64-linux-android"
            EXTRA_CONFIGURE+=("--host=aarch64-linux-android")
            ;;
        android-x86_64)
            _NDK_TRIPLE="x86_64-linux-android"
            EXTRA_CONFIGURE+=("--host=x86_64-linux-android")
            ;;
    esac
    CC="${_NDK_TRIPLE}${ANDROID_API}-clang"
    command -v "$CC" >/dev/null 2>&1 || {
        echo "error: $CC not on PATH (NDK shellHook expected)" >&2
        exit 1
    }
    NEEDS_NATIVE_BOOTSTRAP=1
    ;;

webasm)
    command -v emcc >/dev/null 2>&1 || {
        echo "error: emcc not found — source the .#3rdparty-webasm shell" >&2
        exit 1
    }
    CC=emcc
    EXTRA_CONFIGURE+=("--host=wasm32-unknown-emscripten")
    MAKE_PREFIX="emmake "
    NEEDS_NATIVE_BOOTSTRAP=1
    ;;

windows-x86_64)
    if [ "${MSYSTEM:-}" != "CLANG64" ]; then
        echo "error: windows-x86_64 must run inside MSYS2 CLANG64 (MSYSTEM=$MSYSTEM)" >&2
        exit 1
    fi
    CC=clang
    # MSYS2 CLANG64 is "native" enough for autotools — no --host needed.
    ;;

*)
    echo "unknown TARGET_PLATFORM: $TARGET_PLATFORM" >&2
    exit 1
    ;;
esac

#-----------------------------------------------------------------------------
# Native bootstrap (cross-only): build host's mkmagic so the cross
# `make` can compile magic.mgc from src/Magdir/* without trying to run
# the cross binary.
#-----------------------------------------------------------------------------
NATIVE_FILE_BIN=""
if [ "$NEEDS_NATIVE_BOOTSTRAP" = "1" ]; then
    if [ ! -x "$NATIVE_DIR/src/file" ]; then
        echo "==> native bootstrap (for mkmagic) -> $NATIVE_DIR"
        rm -rf "$NATIVE_DIR"
        cp -a "$SRC_BASE" "$NATIVE_DIR"
        find "$NATIVE_DIR" \
            \( -name "aclocal.m4" -o -name "configure" -o -name "Makefile.in" -o -name "config.h.in" \) \
            -exec touch {} \;
        (
            cd "$NATIVE_DIR"
            ./configure --disable-shared --enable-static \
                --disable-libseccomp --disable-bzlib --disable-xzlib \
                --disable-zstdlib --disable-lzlib --disable-zlib \
                --disable-maintainer-mode \
                --prefix="$NATIVE_DIR/install" >/dev/null
            make -j"$NCPU" >/dev/null
        )
    fi
    NATIVE_FILE_BIN="$NATIVE_DIR/src/file"
    [ -x "$NATIVE_FILE_BIN" ] || {
        echo "native bootstrap failed: $NATIVE_FILE_BIN missing" >&2
        exit 1
    }
fi

#-----------------------------------------------------------------------------
# Configure + build
#-----------------------------------------------------------------------------
echo "==> configuring file-${VERSION} for $TARGET_PLATFORM (CC=$CC)"
(
    cd "$SRC_DIR"
    env "CC=$CC" "CFLAGS=-fPIC -O3 -DNDEBUG" \
        "${CONFIGURE_ENV[@]}" \
        ./configure "${COMMON_FLAGS[@]}" "${EXTRA_CONFIGURE[@]}"
)

echo "==> building (-j${NCPU})"
MAKE_OVERRIDES=()
if [ -n "$NATIVE_FILE_BIN" ]; then
    # Tell the cross make to use the native file binary for compiling
    # magic.mgc. file's Makefile.am uses $(FILE_COMPILE) for this; in 5.x
    # it's hardcoded to ../src/file, but we override at make level.
    MAKE_OVERRIDES+=("FILE_COMPILE=$NATIVE_FILE_BIN")
fi
(
    cd "$SRC_DIR"
    ${MAKE_PREFIX}make -j"$NCPU" "${MAKE_OVERRIDES[@]}"
    ${MAKE_PREFIX}make install "${MAKE_OVERRIDES[@]}"
)

#-----------------------------------------------------------------------------
# Stage + verify
#-----------------------------------------------------------------------------
mkdir -p "$STAGE/lib" "$STAGE/include" "$STAGE/share/misc"
for _D in lib lib64; do
    if [ -d "$INSTALL_DIR/$_D" ]; then
        cp -a "$INSTALL_DIR/$_D/." "$STAGE/lib/"
    fi
done
cp -a "$INSTALL_DIR/include/." "$STAGE/include/"
if [ -f "$INSTALL_DIR/share/misc/magic.mgc" ]; then
    cp -a "$INSTALL_DIR/share/misc/magic.mgc" "$STAGE/share/misc/"
fi

# Drop pkgconfig + share/man + share/file (the latter holds source magic
# files; we only need the compiled .mgc) + .la libtool archives (we ship
# static-only, no need for libtool metadata).
rm -rf "$STAGE/lib/pkgconfig" "$STAGE/share/man" "$STAGE/share/file"
find "$STAGE/lib" -name "*.la" -delete 2>/dev/null || true

# Accept libmagic.a (unix/MSYS2) or magic.lib (Windows native MSVC).
_LIB_FOUND=""
for _CAND in "$STAGE/lib/libmagic.a" "$STAGE/lib/magic.lib"; do
    if [ -f "$_CAND" ]; then _LIB_FOUND="$_CAND"; break; fi
done
if [ -z "$_LIB_FOUND" ]; then
    echo "missing libmagic static lib in stage" >&2
    find "$STAGE" -maxdepth 4 -print >&2 || true
    exit 1
fi
[ -f "$STAGE/include/magic.h" ] || { echo "missing magic.h" >&2; exit 1; }
[ -f "$STAGE/share/misc/magic.mgc" ] || {
    echo "WARNING: magic.mgc not produced — runtime classify will fail" >&2
}

echo "==> packaging -> $TARBALL"
tar -C "$STAGE" -czf "$TARBALL" .

echo ""
echo "libmagic $VERSION ($TARGET_PLATFORM) ready (lib: $(basename "$_LIB_FOUND")):"
ls -lh "$TARBALL"
ENTRIES="$(tar -tzf "$TARBALL" | wc -l)"
echo "contents:"
tar -tzf "$TARBALL"
