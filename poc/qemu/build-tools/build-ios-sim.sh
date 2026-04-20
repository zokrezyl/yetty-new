#!/bin/bash
set -e

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$SRCDIR/build-ios-sim"
LOGDIR="$SRCDIR/tmp"
QEMUSRC="$SRCDIR/qemu-11.0.0-rc4"
DEPSDIR="$SRCDIR/deps-ios-sim"
PREFIX="$DEPSDIR/install"

SIM_SDK="/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator18.2.sdk"
IOS_MIN_VERSION="14.0"
TOOLCHAIN="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"

mkdir -p "$LOGDIR"
mkdir -p "$DEPSDIR"
mkdir -p "$PREFIX"

SIM_FLAGS="-arch x86_64 -isysroot $SIM_SDK -mios-simulator-version-min=$IOS_MIN_VERSION"

export CC="$TOOLCHAIN/clang"
export CXX="$TOOLCHAIN/clang++"
export AR="$TOOLCHAIN/ar"
export RANLIB="$TOOLCHAIN/ranlib"
export STRIP="$TOOLCHAIN/strip"
export NM="$TOOLCHAIN/nm"
export CFLAGS="$SIM_FLAGS"
export CXXFLAGS="$SIM_FLAGS"
export LDFLAGS="$SIM_FLAGS"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PATH="$PREFIX/bin:$PATH"

NCPU=$(sysctl -n hw.ncpu)

# Build libffi
LIBFFI_VER="3.4.6"
if [ ! -f "$PREFIX/lib/libffi.a" ]; then
    echo "Building libffi..."
    cd "$DEPSDIR"
    [ ! -f "libffi-$LIBFFI_VER.tar.gz" ] && curl -LO "https://github.com/libffi/libffi/releases/download/v$LIBFFI_VER/libffi-$LIBFFI_VER.tar.gz"
    rm -rf "libffi-$LIBFFI_VER" && tar xzf "libffi-$LIBFFI_VER.tar.gz"
    cd "libffi-$LIBFFI_VER"
    ./configure --host=x86_64-apple-darwin --prefix="$PREFIX" --disable-shared --enable-static \
        > "$LOGDIR/libffi-sim-configure.log" 2>&1
    make -j$NCPU > "$LOGDIR/libffi-sim-build.log" 2>&1
    make install > "$LOGDIR/libffi-sim-install.log" 2>&1
    echo "libffi done"
fi

# Build pcre2
PCRE2_VER="10.44"
if [ ! -f "$PREFIX/lib/libpcre2-8.a" ]; then
    echo "Building pcre2..."
    cd "$DEPSDIR"
    [ ! -f "pcre2-$PCRE2_VER.tar.gz" ] && curl -LO "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-$PCRE2_VER/pcre2-$PCRE2_VER.tar.gz"
    rm -rf "pcre2-$PCRE2_VER" && tar xzf "pcre2-$PCRE2_VER.tar.gz"
    cd "pcre2-$PCRE2_VER"
    ./configure --host=x86_64-apple-darwin --prefix="$PREFIX" --disable-shared --enable-static \
        > "$LOGDIR/pcre2-sim-configure.log" 2>&1
    make -j$NCPU > "$LOGDIR/pcre2-sim-build.log" 2>&1
    make install > "$LOGDIR/pcre2-sim-install.log" 2>&1
    echo "pcre2 done"
fi

# Build glib
GLIB_VER="2.82.4"
if [ ! -f "$PREFIX/lib/libglib-2.0.a" ]; then
    echo "Building glib..."
    cd "$DEPSDIR"
    [ ! -f "glib-$GLIB_VER.tar.xz" ] && curl -LO "https://download.gnome.org/sources/glib/2.82/glib-$GLIB_VER.tar.xz"
    rm -rf "glib-$GLIB_VER" && tar xJf "glib-$GLIB_VER.tar.xz"
    cd "glib-$GLIB_VER"

    cat > sim-cross.txt << EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkgconfig = 'pkg-config'

[built-in options]
c_args = ['-arch', 'x86_64', '-isysroot', '$SIM_SDK', '-mios-simulator-version-min=$IOS_MIN_VERSION']
c_link_args = ['-arch', 'x86_64', '-isysroot', '$SIM_SDK', '-mios-simulator-version-min=$IOS_MIN_VERSION']
cpp_args = ['-arch', 'x86_64', '-isysroot', '$SIM_SDK', '-mios-simulator-version-min=$IOS_MIN_VERSION']
cpp_link_args = ['-arch', 'x86_64', '-isysroot', '$SIM_SDK', '-mios-simulator-version-min=$IOS_MIN_VERSION']

[properties]
needs_exe_wrapper = true
# Cross-compile overrides for glib gnulib
have_c99_vsnprintf = true
have_c99_snprintf = true
have_unix98_printf = true
growing_stack = false

[host_machine]
system = 'darwin'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

    meson setup build-sim \
        --cross-file=sim-cross.txt \
        --prefix="$PREFIX" \
        --default-library=static \
        -Dtests=false \
        -Dintrospection=disabled \
        -Dnls=disabled \
        -Dlibmount=disabled \
        -Dglib_debug=disabled \
        -Dlibelf=disabled \
        -Dsysprof=disabled \
        > "$LOGDIR/glib-sim-configure.log" 2>&1

    ninja -C build-sim > "$LOGDIR/glib-sim-build.log" 2>&1
    ninja -C build-sim install > "$LOGDIR/glib-sim-install.log" 2>&1
    echo "glib done"
fi

# Build QEMU
echo "Building QEMU..."
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

unset CFLAGS CXXFLAGS LDFLAGS
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"

"$QEMUSRC/configure" \
  --cross-prefix="" \
  --cc="$CC" \
  --cxx="$CXX" \
  --objcc="$CC" \
  --cpu=x86_64 \
  --target-list=x86_64-softmmu,riscv64-softmmu \
  --without-default-features \
  --enable-tcg \
  --enable-slirp \
  --enable-virtfs \
  --disable-werror \
  --disable-docs \
  --disable-guest-agent \
  --disable-tools \
  --extra-cflags="$SIM_FLAGS -I$PREFIX/include -I$PREFIX/include/glib-2.0 -I$PREFIX/lib/glib-2.0/include" \
  --extra-cxxflags="$SIM_FLAGS -I$PREFIX/include -I$PREFIX/include/glib-2.0 -I$PREFIX/lib/glib-2.0/include" \
  --extra-objcflags="$SIM_FLAGS" \
  --extra-ldflags="$SIM_FLAGS -L$PREFIX/lib" \
  > "$LOGDIR/configure-ios-sim.log" 2>&1

echo "Configure done. Building..."

make -j$NCPU > "$LOGDIR/build-ios-sim.log" 2>&1

echo "Build complete: $BUILDDIR/qemu-system-x86_64"
