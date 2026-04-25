#!/bin/bash
# Builds qemu-system-riscv64 for $TARGET_PLATFORM and packages it as a
# tarball under $OUTPUT_DIR. All per-platform logic lives inline below
# in a case block — no separate platform scripts (except windows, which
# is big enough to justify its own file at platforms/windows-x86_64.sh).
#
# Required env:
#   TARGET_PLATFORM   linux-x86_64 | linux-aarch64 |
#                     android-arm64-v8a | android-x86_64 |
#                     macos-arm64 | macos-x86_64 |
#                     ios-arm64 | ios-x86_64 | tvos-arm64 |
#                     windows-x86_64
#   VERSION           e.g. 0.0.1
#   OUTPUT_DIR        where the tarball is written
# Optional env:
#   WORK_DIR          default /tmp/yetty-asset-qemu-$TARGET_PLATFORM
#   CACHE_DIR         default $HOME/.cache/yetty-qemu-assets
#                     holds the QEMU source tarball so multi-target builds
#                     share a single download
#   QEMU_VERSION      default 11.0.0-rc4
#
# QEMU configure flags are kept in sync with build-tools/cmake/qemu.cmake.

set -Eeuo pipefail
trap 'rc=$?; echo "FAILED: rc=$rc line=$LINENO source=${BASH_SOURCE[0]} cmd: $BASH_COMMAND" >&2' ERR

: "${TARGET_PLATFORM:?TARGET_PLATFORM is required}"
: "${VERSION:?VERSION is required}"
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
WORK_DIR="${WORK_DIR:-/tmp/yetty-asset-qemu-$TARGET_PLATFORM}"
CACHE_DIR="${CACHE_DIR:-$HOME/.cache/yetty-qemu-assets}"
QEMU_VERSION="${QEMU_VERSION:-11.0.0-rc4}"
NCPU="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"

QEMU_URL="https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz"
QEMU_TARBALL="$CACHE_DIR/qemu-${QEMU_VERSION}.tar.xz"
SRC_DIR="$WORK_DIR/qemu-${QEMU_VERSION}"
BUILD_DIR="$WORK_DIR/build-${TARGET_PLATFORM}"
STAGE="$WORK_DIR/stage-${TARGET_PLATFORM}"
TARBALL="$OUTPUT_DIR/qemu-${TARGET_PLATFORM}-${VERSION}.tar.gz"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR" "$CACHE_DIR"
cd "$WORK_DIR"

#-----------------------------------------------------------------------------
# Fetch (shared across targets) + extract QEMU source
#-----------------------------------------------------------------------------
if [ ! -f "$QEMU_TARBALL" ]; then
    # Serialize with a flock so parallel per-target builds share one
    # download. Per-PID .part file avoids clobbering if the lock is
    # unavailable (e.g. no flock on this host — fall through harmlessly).
    _part="$QEMU_TARBALL.part.$$"
    (
        if command -v flock >/dev/null 2>&1; then
            flock -x 9
        fi
        if [ ! -f "$QEMU_TARBALL" ]; then
            echo "==> downloading QEMU ${QEMU_VERSION} to cache ($QEMU_TARBALL)"
            curl -fL --retry 3 -o "$_part" "$QEMU_URL"
            mv "$_part" "$QEMU_TARBALL"
        fi
    ) 9>"$CACHE_DIR/.qemu-download.lock"
    rm -f "$_part"
else
    echo "==> using cached QEMU tarball: $QEMU_TARBALL"
fi
if [ ! -f "$SRC_DIR/configure" ]; then
    echo "==> extracting QEMU"
    # Skip roms/ (firmware source trees: u-boot, edk2, skiboot — heavy
    # symlink churn that breaks extraction on Windows without Developer
    # Mode). riscv64-softmmu doesn't need them — it uses pre-built blobs
    # from pc-bios/.
    # Skip tests/lcitool/libvirt-ci too (nested submodule with prep-script
    # symlinks). Keep tests/lcitool/Makefile.include — QEMU's top-level
    # Makefile unconditionally `include`s it.
    tar xf "$QEMU_TARBALL" \
        --exclude='qemu-*/roms' \
        --exclude='qemu-*/tests/lcitool/libvirt-ci'
fi

# Pruned device config (shared with poc/qemu)
DEVCFG_DIR="$SRC_DIR/configs/devices/riscv64-softmmu"
mkdir -p "$DEVCFG_DIR"
cp "$REPO_ROOT/poc/qemu/configs/riscv64-softmmu/default.mak" "$DEVCFG_DIR/default.mak"

#-----------------------------------------------------------------------------
# Common configure flags — mirror build-tools/cmake/qemu.cmake.
#-----------------------------------------------------------------------------
_CONFIGURE_ARGS=(
    --target-list=riscv64-softmmu
    --without-default-features
    --enable-tcg
    --enable-slirp
    --enable-virtfs
    --enable-fdt=internal
    --enable-trace-backends=nop
    --disable-werror
    --disable-docs
    --disable-guest-agent
    --disable-tools
    --disable-qom-cast-debug
    --disable-coroutine-pool
)
_EXTRA_CFLAGS="-Os -ffunction-sections -fdata-sections"
_EXTRA_CXXFLAGS="-Os -ffunction-sections -fdata-sections"
_EXTRA_LDFLAGS="-Wl,--gc-sections"
_QEMU_BINARY_NAME="qemu-system-riscv64"
_QEMU_OUTPUT_NAME=""     # defaults to _QEMU_BINARY_NAME at packaging time
_STRIP_BIN="strip"

#-----------------------------------------------------------------------------
# Per-platform block: sets toolchain/SDK flags and, where needed, builds a
# dependency sysroot (android) or patches QEMU's configure (windows).
#-----------------------------------------------------------------------------
case "$TARGET_PLATFORM" in

linux-x86_64)
    _CONFIGURE_ARGS+=(--enable-attr --cc=gcc --cxx=g++)
    ;;

linux-aarch64)
    : "${CROSS_PREFIX:=aarch64-unknown-linux-gnu-}"
    _CONFIGURE_ARGS+=(
        --enable-attr
        --cross-prefix="$CROSS_PREFIX"
        --cc="${CROSS_PREFIX}gcc"
        --cxx="${CROSS_PREFIX}g++"
        --host-cc=gcc
    )
    _STRIP_BIN="${CROSS_PREFIX}strip"
    ;;

android-arm64-v8a|android-x86_64)
    # NDK-direct cross build. The .#assets-qemu-android-* nix shell must
    # put the NDK triple-prefixed clang on PATH and export ANDROID_NDK_HOME.
    : "${ANDROID_API:=28}"   # bionic gained iconv at API 28, glib needs it
    : "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME not set — source the .#assets-qemu-android-* shell}"

    case "$TARGET_PLATFORM" in
        android-arm64-v8a) ANDROID_TRIPLE="aarch64-linux-android"; ANDROID_CPU="aarch64" ;;
        android-x86_64)    ANDROID_TRIPLE="x86_64-linux-android";  ANDROID_CPU="x86_64"  ;;
    esac

    _CC="${ANDROID_TRIPLE}${ANDROID_API}-clang"
    _CXX="${ANDROID_TRIPLE}${ANDROID_API}-clang++"
    command -v "$_CC" >/dev/null || {
        echo "error: $_CC not on PATH (expected via NDK shellHook)" >&2
        exit 1
    }

    # Build pcre2/libffi/glib/pixman into a per-ABI sysroot. nixpkgs
    # pkgsCross.*-android hits a compiler-rt/pthread.h regression on
    # clang 19+, so avoid it entirely.
    PCRE2_VERSION="10.44"
    LIBFFI_VERSION="3.4.6"
    GLIB_VERSION="2.80.5"
    PIXMAN_VERSION="0.44.0"
    SYSROOT="$WORK_DIR/android-sysroot-${TARGET_PLATFORM##android-}"
    SYSROOT_STAMP="$SYSROOT/.built-$PCRE2_VERSION-$LIBFFI_VERSION-$GLIB_VERSION-$PIXMAN_VERSION"
    DEPS_DIR="$WORK_DIR/android-deps-src"
    mkdir -p "$SYSROOT" "$DEPS_DIR"

    CROSSFILE="$SYSROOT/android-${TARGET_PLATFORM##android-}.ini"
    cat > "$CROSSFILE" <<CROSS_EOF
[binaries]
c         = '$_CC'
cpp       = '$_CXX'
ar        = 'llvm-ar'
strip     = 'llvm-strip'
ranlib    = 'llvm-ranlib'
pkg-config = 'pkg-config'

[host_machine]
system     = 'android'
cpu_family = '$ANDROID_CPU'
cpu        = '$ANDROID_CPU'
endian     = 'little'
CROSS_EOF

    _fetch() {
        local url="$1" out="$2"
        if [ ! -f "$DEPS_DIR/$out" ]; then
            echo "  fetch $out"
            curl -fL --retry 3 -o "$DEPS_DIR/$out.part" "$url"
            mv "$DEPS_DIR/$out.part" "$DEPS_DIR/$out"
        fi
    }

    _autotools_build() {
        local name="$1" src="$2"
        echo "==> android sysroot: $name"
        (
            cd "$src"
            if [ ! -f "build-${TARGET_PLATFORM}/Makefile" ]; then
                rm -rf "build-${TARGET_PLATFORM}"
                mkdir -p "build-${TARGET_PLATFORM}"
                (
                    cd "build-${TARGET_PLATFORM}"
                    CC="$_CC" AR=llvm-ar RANLIB=llvm-ranlib \
                        ../configure --host="$ANDROID_TRIPLE" \
                            --prefix="$SYSROOT" \
                            --disable-shared --enable-static
                )
            fi
            cd "build-${TARGET_PLATFORM}"
            make -j"$NCPU"
            make install
        )
    }

    _meson_build() {
        local name="$1" src="$2"; shift 2
        echo "==> android sysroot: $name"
        rm -rf "$DEPS_DIR/$name-build"
        # No --wrap-mode=nodownload: glib requires the proxy-libintl wrap
        # subproject on systems without libintl (Android bionic), and
        # meson needs to fetch it via the wrap DB.
        meson setup "$DEPS_DIR/$name-build" "$src" \
            --cross-file="$CROSSFILE" \
            --prefix="$SYSROOT" \
            --buildtype=release \
            --default-library=static \
            "$@"
        meson install -C "$DEPS_DIR/$name-build"
    }

    if [ -f "$SYSROOT_STAMP" ]; then
        echo "==> android sysroot already built: $SYSROOT"
    else
        echo "==> building android sysroot (pcre2, libffi, glib, pixman) for $TARGET_PLATFORM"

        # pcre2 — glib hard-dep. 10.44's tarball ships autotools only.
        _fetch "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${PCRE2_VERSION}/pcre2-${PCRE2_VERSION}.tar.bz2" \
            "pcre2-${PCRE2_VERSION}.tar.bz2"
        [ -d "$DEPS_DIR/pcre2-${PCRE2_VERSION}" ] || tar -C "$DEPS_DIR" -xjf "$DEPS_DIR/pcre2-${PCRE2_VERSION}.tar.bz2"
        _autotools_build "pcre2" "$DEPS_DIR/pcre2-${PCRE2_VERSION}"

        # libffi — glib hard-dep.
        _fetch "https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/libffi-${LIBFFI_VERSION}.tar.gz" \
            "libffi-${LIBFFI_VERSION}.tar.gz"
        [ -d "$DEPS_DIR/libffi-${LIBFFI_VERSION}" ] || tar -C "$DEPS_DIR" -xzf "$DEPS_DIR/libffi-${LIBFFI_VERSION}.tar.gz"
        _autotools_build "libffi" "$DEPS_DIR/libffi-${LIBFFI_VERSION}"

        # glib — QEMU's main dep (meson).
        GLIB_MINOR="${GLIB_VERSION%.*}"
        _fetch "https://download.gnome.org/sources/glib/${GLIB_MINOR}/glib-${GLIB_VERSION}.tar.xz" \
            "glib-${GLIB_VERSION}.tar.xz"
        [ -d "$DEPS_DIR/glib-${GLIB_VERSION}" ] || tar -C "$DEPS_DIR" -xJf "$DEPS_DIR/glib-${GLIB_VERSION}.tar.xz"
        export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/lib64/pkgconfig"
        # Empty, not $SYSROOT: the .pc files we just installed via
        # `--prefix=$SYSROOT` contain absolute paths already; a non-empty
        # PKG_CONFIG_SYSROOT_DIR would double-prefix them.
        export PKG_CONFIG_SYSROOT_DIR=""
        _meson_build "glib" "$DEPS_DIR/glib-${GLIB_VERSION}" \
            -Dtests=false \
            -Dinstalled_tests=false \
            -Dnls=disabled \
            -Dselinux=disabled \
            -Dxattr=false \
            -Dlibmount=disabled \
            -Dintrospection=disabled \
            -Ddocumentation=false \
            -Dman-pages=disabled \
            -Dsysprof=disabled \
            -Doss_fuzz=disabled \
            -Dglib_debug=disabled

        # pixman (meson).
        _fetch "https://cairographics.org/releases/pixman-${PIXMAN_VERSION}.tar.gz" \
            "pixman-${PIXMAN_VERSION}.tar.gz"
        [ -d "$DEPS_DIR/pixman-${PIXMAN_VERSION}" ] || tar -C "$DEPS_DIR" -xzf "$DEPS_DIR/pixman-${PIXMAN_VERSION}.tar.gz"
        _meson_build "pixman" "$DEPS_DIR/pixman-${PIXMAN_VERSION}" \
            -Dtests=disabled \
            -Ddemos=disabled

        touch "$SYSROOT_STAMP"
        echo "==> android sysroot ready: $SYSROOT"
    fi

    export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/lib64/pkgconfig"
    export PKG_CONFIG_SYSROOT_DIR=""   # deps already under absolute sysroot
    export AR="llvm-ar"
    export STRIP="llvm-strip"
    export RANLIB="llvm-ranlib"
    export NM="llvm-nm"
    export OBJCOPY="llvm-objcopy"

    # QEMU's configure with --cross-prefix looks for `<prefix>pkg-config`.
    # We only have plain pkg-config, so drop a shim on PATH that forwards.
    mkdir -p "$SYSROOT/bin"
    _PKGCONFIG_SHIM="$SYSROOT/bin/${ANDROID_TRIPLE}${ANDROID_API}-pkg-config"
    [ -e "$_PKGCONFIG_SHIM" ] || ln -s "$(command -v pkg-config)" "$_PKGCONFIG_SHIM"
    export PATH="$SYSROOT/bin:$PATH"

    # Bionic doesn't ship shm_open for arm64/x86_64 (only arm/i386), and
    # there's no standalone librt on Android. QEMU's meson.build:1361
    # hard-requires librt when shm_open isn't found — patch it to
    # required:false so the detection degrades gracefully.
    if grep -q "rt = cc.find_library('rt', required: true)" "$SRC_DIR/meson.build"; then
        sed -i "s|rt = cc.find_library('rt', required: true)|rt = cc.find_library('rt', required: false)|" \
            "$SRC_DIR/meson.build"
        echo "==> patched $SRC_DIR/meson.build: rt library required:false for android"
    fi

    # util/oslib-posix.c:qemu_shm_alloc() calls shm_open/shm_unlink which
    # bionic doesn't declare for arm64/x86_64. Replace the whole body on
    # Android — riscv64-softmmu with --without-default-features never
    # exercises POSIX shm at runtime.
    _OSLIB="$SRC_DIR/util/oslib-posix.c"
    if grep -q "^    fd = shm_open(shm_name->str" "$_OSLIB"; then
        sed -i '/^int qemu_shm_alloc(size_t size, Error \*\*errp)$/,/^}$/c\
int qemu_shm_alloc(size_t size, Error **errp)\
{\
    /* Android stub: bionic lacks shm_open/shm_unlink for arm64/x86_64. */\
    (void)size;\
    error_setg_errno(errp, ENOSYS, "POSIX shm not supported on this platform");\
    return -1;\
}' "$_OSLIB"
        echo "==> patched $_OSLIB: qemu_shm_alloc replaced with Android stub"
    fi

    # fsdev/9p-marshal.h declares `struct V9fsStatDotl` with members
    # st_atime_nsec / st_mtime_nsec / st_ctime_nsec (plus st_atimensec
    # variants) that collide with bionic's <sys/stat.h> macros
    # (st_atime_nsec -> st_atim.tv_nsec etc.). Undef only the _nsec
    # macros — keep st_atime/st_mtime/st_ctime defined because 9pfs code
    # (hw/9pfs/9p.c, 9p-synth.c) reads those from `struct stat`, and
    # bionic's struct stat has no st_atime member, only the macro alias.
    # No code anywhere accesses struct stat via the *_nsec names, so a
    # global undef of just those six is safe.
    _P9H="$SRC_DIR/fsdev/9p-marshal.h"
    if ! grep -q "ANDROID st_ _nsec macro undefs" "$_P9H"; then
        sed -i '/^#include "p9array.h"$/a\
\
/* ANDROID st_ _nsec macro undefs — bionic <sys/stat.h> defines these as\
 * struct-stat access-path macros, and they collide with V9fsStatDotl\
 * member names. st_atime/st_mtime/st_ctime stay defined. */\
#ifdef __ANDROID__\
# undef st_atimensec\
# undef st_mtimensec\
# undef st_ctimensec\
# undef st_atime_nsec\
# undef st_mtime_nsec\
# undef st_ctime_nsec\
#endif' "$_P9H"
        echo "==> patched $_P9H: undef bionic st_*_nsec macros"
    fi

    _EXTRA_CFLAGS="-Os -ffunction-sections -fdata-sections -I$SYSROOT/include"
    _EXTRA_CXXFLAGS="$_EXTRA_CFLAGS"
    _EXTRA_LDFLAGS="-Wl,--gc-sections -L$SYSROOT/lib"

    _CONFIGURE_ARGS+=(
        --cross-prefix="${ANDROID_TRIPLE}${ANDROID_API}-"
        --cc="$_CC"
        --cxx="$_CXX"
        --host-cc=gcc
        # --without-default-features turns every auto feature off; virtfs
        # needs attr, and bionic satisfies the attr test via in-libc
        # getxattr/setxattr (QEMU's libattr_test links without -lattr).
        --enable-attr
    )
    _STRIP_BIN="llvm-strip"
    ;;

macos-arm64|macos-x86_64)
    # Native on macOS runner. CI installs prereqs via brew
    # (ninja meson pkg-config glib pixman).
    case "$TARGET_PLATFORM" in
        macos-arm64)  _ARCH="arm64"  ;;
        macos-x86_64) _ARCH="x86_64" ;;
    esac
    _CONFIGURE_ARGS+=(
        --cc=clang
        --cxx=clang++
        --extra-cflags="-arch $_ARCH"
        --extra-cxxflags="-arch $_ARCH"
    )
    _EXTRA_LDFLAGS="-Wl,-dead_strip -arch $_ARCH"
    _QEMU_BINARY_NAME="qemu-system-riscv64-unsigned"
    _QEMU_OUTPUT_NAME="qemu-system-riscv64"
    ;;

ios-arm64|ios-x86_64|tvos-arm64)
    # Cross from macOS. The nix shell puts xcbuild's stub xcrun on PATH —
    # call Apple's /usr/bin/xcrun directly to resolve Xcode SDK paths.
    # The shell also exports DEVELOPER_DIR=<nix apple-sdk> which /usr/bin/xcrun
    # honors and would search for iphoneos/appletvos SDKs there (not present);
    # unset so xcrun falls back to `xcode-select -p` (real Xcode.app).
    # The nix apple-sdk shell also exports MACOSX_DEPLOYMENT_TARGET/SDKROOT and
    # configures `clang` on PATH as a wrapper that hard-injects
    # `-mmacos-version-min=<that target>`, which is incompatible with
    # `-mios-version-min=...`. Use Apple's /usr/bin/clang directly and strip
    # the env vars so xcrun + our -isysroot fully determine the target.
    unset DEVELOPER_DIR MACOSX_DEPLOYMENT_TARGET SDKROOT NIX_APPLE_SDK_VERSION

    # Meson also auto-detects an Objective-C compiler for Darwin targets and
    # defaults to `clang` on PATH — which is nix's wrapper. Force Apple's too.
    export OBJC=/usr/bin/clang
    export OBJCXX=/usr/bin/clang++
    case "$TARGET_PLATFORM" in
        ios-arm64)
            _SDK_NAME="iphoneos";         _ARCH="arm64"
            _MIN_FLAG="-mios-version-min=${IOS_MIN_VERSION:-15.0}"
            _EXTRA_QCFG=(--cross-prefix="")
            ;;
        ios-x86_64)
            _SDK_NAME="iphonesimulator";  _ARCH="x86_64"
            _MIN_FLAG="-mios-simulator-version-min=${IOS_MIN_VERSION:-15.0}"
            _EXTRA_QCFG=()
            ;;
        tvos-arm64)
            _SDK_NAME="appletvos";        _ARCH="arm64"
            _MIN_FLAG="-mtvos-version-min=${TVOS_MIN_VERSION:-17.0}"
            _EXTRA_QCFG=()
            ;;
    esac
    _SDK="$(/usr/bin/xcrun --sdk "$_SDK_NAME" --show-sdk-path)"
    _DARWIN_CFLAGS="-isysroot $_SDK -arch $_ARCH $_MIN_FLAG"
    _CONFIGURE_ARGS+=(
        --cc=/usr/bin/clang
        --cxx=/usr/bin/clang++
        --objcc=/usr/bin/clang
        --host-cc=/usr/bin/clang
        "${_EXTRA_QCFG[@]}"
        --extra-cflags="$_DARWIN_CFLAGS"
        --extra-cxxflags="$_DARWIN_CFLAGS"
    )
    _EXTRA_LDFLAGS="-Wl,-dead_strip $_DARWIN_CFLAGS"
    _QEMU_BINARY_NAME="qemu-system-riscv64-unsigned"
    _QEMU_OUTPUT_NAME="qemu-system-riscv64"
    ;;

windows-x86_64)
    # Windows is MSVC/clang-cl + vcpkg — kept in its own file because of
    # the size of the pkgconf/patching logic.
    # shellcheck source=platforms/windows-x86_64.sh
    source "$SCRIPT_DIR/platforms/windows-x86_64.sh"
    ;;

*)
    echo "unknown TARGET_PLATFORM: $TARGET_PLATFORM" >&2
    exit 1
    ;;
esac

#-----------------------------------------------------------------------------
# Append compiler/linker extra flags and force bundled slirp to static.
#-----------------------------------------------------------------------------
_CONFIGURE_ARGS+=(
    --extra-cflags="$_EXTRA_CFLAGS"
    --extra-cxxflags="$_EXTRA_CXXFLAGS"
    --extra-ldflags="$_EXTRA_LDFLAGS"
)
# libslirp is the one dep end-user Linuxes can't be relied on to have —
# pull it into the qemu binary instead of shipping subprojects/slirp.so.
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
