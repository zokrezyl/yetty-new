# Android QEMU platform helper: NDK-direct toolchain + builds glib/pixman
# into a per-ABI sysroot from source. Sourced by android-<abi>.sh; the
# caller must have set:
#   ANDROID_TRIPLE   — e.g. aarch64-linux-android or x86_64-linux-android
#   ANDROID_CPU      — meson host_machine cpu_family (aarch64 / x86_64)
#   ANDROID_API      — e.g. 26 (from the .#assets-qemu-android-* shell)
#   ANDROID_NDK_HOME — from the .#assets-qemu-android-* shell
#
# This file is intentionally self-contained and uses only the tools
# already on PATH in the android nix shell (meson, ninja, curl, tar, perl).

: "${ANDROID_TRIPLE:?}"
: "${ANDROID_CPU:?}"
: "${ANDROID_API:=26}"
: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME not set — source the .#assets-qemu-android-* shell}"

_CC="${ANDROID_TRIPLE}${ANDROID_API}-clang"
_CXX="${ANDROID_TRIPLE}${ANDROID_API}-clang++"
command -v "$_CC" >/dev/null || {
    echo "error: $_CC not on PATH (expected via NDK shellHook)" >&2
    exit 1
}

# Pinned versions — bump deliberately, all three tarballs are reproducible.
PCRE2_VERSION="10.44"
LIBFFI_VERSION="3.4.6"
GLIB_VERSION="2.80.5"
PIXMAN_VERSION="0.44.0"

SYSROOT="$WORK_DIR/android-sysroot-${TARGET_PLATFORM##android-}"
SYSROOT_STAMP="$SYSROOT/.built-$PCRE2_VERSION-$LIBFFI_VERSION-$GLIB_VERSION-$PIXMAN_VERSION"
DEPS_DIR="$WORK_DIR/android-deps-src"
mkdir -p "$SYSROOT" "$DEPS_DIR"

# Meson cross-file used by each dependency build.
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

_meson_build() {
    local name="$1" src="$2"; shift 2
    echo "==> android sysroot: $name"
    rm -rf "$DEPS_DIR/$name-build"
    meson setup "$DEPS_DIR/$name-build" "$src" \
        --cross-file="$CROSSFILE" \
        --prefix="$SYSROOT" \
        --buildtype=release \
        --default-library=static \
        --wrap-mode=nodownload \
        "$@"
    meson install -C "$DEPS_DIR/$name-build"
}

if [ -f "$SYSROOT_STAMP" ]; then
    echo "==> android sysroot already built: $SYSROOT"
else
    echo "==> building android sysroot (pcre2, libffi, glib, pixman) for $TARGET_PLATFORM"

    # pcre2 — glib hard-dep
    _fetch "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-${PCRE2_VERSION}/pcre2-${PCRE2_VERSION}.tar.bz2" \
        "pcre2-${PCRE2_VERSION}.tar.bz2"
    if [ ! -d "$DEPS_DIR/pcre2-${PCRE2_VERSION}" ]; then
        tar -C "$DEPS_DIR" -xjf "$DEPS_DIR/pcre2-${PCRE2_VERSION}.tar.bz2"
    fi
    _meson_build "pcre2" "$DEPS_DIR/pcre2-${PCRE2_VERSION}" \
        -Dgrep=false -Dtest=false

    # libffi — glib hard-dep
    _fetch "https://github.com/libffi/libffi/releases/download/v${LIBFFI_VERSION}/libffi-${LIBFFI_VERSION}.tar.gz" \
        "libffi-${LIBFFI_VERSION}.tar.gz"
    if [ ! -d "$DEPS_DIR/libffi-${LIBFFI_VERSION}" ]; then
        tar -C "$DEPS_DIR" -xzf "$DEPS_DIR/libffi-${LIBFFI_VERSION}.tar.gz"
    fi
    # libffi uses autotools, not meson. Build manually.
    (
        cd "$DEPS_DIR/libffi-${LIBFFI_VERSION}"
        if [ ! -f "build-${TARGET_PLATFORM}/Makefile" ]; then
            rm -rf "build-${TARGET_PLATFORM}"
            mkdir -p "build-${TARGET_PLATFORM}"
            cd "build-${TARGET_PLATFORM}"
            CC="$_CC" AR=llvm-ar RANLIB=llvm-ranlib \
                ../configure --host="$ANDROID_TRIPLE" \
                    --prefix="$SYSROOT" \
                    --disable-shared --enable-static
        fi
        cd "build-${TARGET_PLATFORM}"
        make -j"$NCPU"
        make install
    )

    # glib — Qemu's main dep
    GLIB_MINOR="${GLIB_VERSION%.*}"
    _fetch "https://download.gnome.org/sources/glib/${GLIB_MINOR}/glib-${GLIB_VERSION}.tar.xz" \
        "glib-${GLIB_VERSION}.tar.xz"
    if [ ! -d "$DEPS_DIR/glib-${GLIB_VERSION}" ]; then
        tar -C "$DEPS_DIR" -xJf "$DEPS_DIR/glib-${GLIB_VERSION}.tar.xz"
    fi
    # Point glib's pkg-config lookups at the sysroot we just populated.
    export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/lib64/pkgconfig"
    export PKG_CONFIG_SYSROOT_DIR="$SYSROOT"
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

    # pixman
    _fetch "https://cairographics.org/releases/pixman-${PIXMAN_VERSION}.tar.gz" \
        "pixman-${PIXMAN_VERSION}.tar.gz"
    if [ ! -d "$DEPS_DIR/pixman-${PIXMAN_VERSION}" ]; then
        tar -C "$DEPS_DIR" -xzf "$DEPS_DIR/pixman-${PIXMAN_VERSION}.tar.gz"
    fi
    _meson_build "pixman" "$DEPS_DIR/pixman-${PIXMAN_VERSION}" \
        -Dtests=disabled \
        -Ddemos=disabled

    touch "$SYSROOT_STAMP"
    echo "==> android sysroot ready: $SYSROOT"
fi

# Point QEMU's configure at the freshly-built sysroot.
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig:$SYSROOT/lib64/pkgconfig"
export PKG_CONFIG_SYSROOT_DIR=""     # don't prepend sysroot — deps already absolute
export AR="llvm-ar"
export STRIP="llvm-strip"
export RANLIB="llvm-ranlib"
export NM="llvm-nm"
export OBJCOPY="llvm-objcopy"

_EXTRA_CFLAGS="-Os -ffunction-sections -fdata-sections -I$SYSROOT/include"
_EXTRA_CXXFLAGS="$_EXTRA_CFLAGS"
_EXTRA_LDFLAGS="-Wl,--gc-sections -L$SYSROOT/lib"

_CONFIGURE_ARGS+=(
    --cross-prefix="${ANDROID_TRIPLE}${ANDROID_API}-"
    --cc="$_CC"
    --cxx="$_CXX"
    --host-cc=gcc
)
_STRIP_BIN="llvm-strip"
