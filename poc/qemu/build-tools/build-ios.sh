#!/bin/bash
set -e

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$SRCDIR/build-ios"
LOGDIR="$SRCDIR/tmp"
QEMUSRC="$SRCDIR/qemu-11.0.0-rc4"
DEPSDIR="$SRCDIR/deps-ios"
PREFIX="$DEPSDIR/install"

IOS_SDK="/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS18.2.sdk"
IOS_MIN_VERSION="14.0"
TOOLCHAIN="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"

mkdir -p "$LOGDIR"
mkdir -p "$DEPSDIR"
mkdir -p "$PREFIX"

IOS_FLAGS="-arch arm64 -isysroot $IOS_SDK -miphoneos-version-min=$IOS_MIN_VERSION"

export CC="$TOOLCHAIN/clang"
export CXX="$TOOLCHAIN/clang++"
export AR="$TOOLCHAIN/ar"
export RANLIB="$TOOLCHAIN/ranlib"
export STRIP="$TOOLCHAIN/strip"
export NM="$TOOLCHAIN/nm"
export CFLAGS="$IOS_FLAGS"
export CXXFLAGS="$IOS_FLAGS"
export LDFLAGS="$IOS_FLAGS"
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
    ./configure --host=aarch64-apple-darwin --prefix="$PREFIX" --disable-shared --enable-static \
        > "$LOGDIR/libffi-configure.log" 2>&1
    make -j$NCPU > "$LOGDIR/libffi-build.log" 2>&1
    make install > "$LOGDIR/libffi-install.log" 2>&1
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
    ./configure --host=aarch64-apple-darwin --prefix="$PREFIX" --disable-shared --enable-static \
        > "$LOGDIR/pcre2-configure.log" 2>&1
    make -j$NCPU > "$LOGDIR/pcre2-build.log" 2>&1
    make install > "$LOGDIR/pcre2-install.log" 2>&1
    echo "pcre2 done"
fi

# Build glib (meson based)
GLIB_VER="2.82.4"
if [ ! -f "$PREFIX/lib/libglib-2.0.a" ]; then
    echo "Building glib..."
    cd "$DEPSDIR"
    [ ! -f "glib-$GLIB_VER.tar.xz" ] && curl -LO "https://download.gnome.org/sources/glib/2.82/glib-$GLIB_VER.tar.xz"
    rm -rf "glib-$GLIB_VER" && tar xJf "glib-$GLIB_VER.tar.xz"
    cd "glib-$GLIB_VER"

    # Create meson cross file for iOS
    cat > ios-cross.txt << EOF
[binaries]
c = '$CC'
cpp = '$CXX'
ar = '$AR'
strip = '$STRIP'
pkgconfig = 'pkg-config'

[built-in options]
c_args = ['-arch', 'arm64', '-isysroot', '$IOS_SDK', '-miphoneos-version-min=$IOS_MIN_VERSION']
c_link_args = ['-arch', 'arm64', '-isysroot', '$IOS_SDK', '-miphoneos-version-min=$IOS_MIN_VERSION']
cpp_args = ['-arch', 'arm64', '-isysroot', '$IOS_SDK', '-miphoneos-version-min=$IOS_MIN_VERSION']
cpp_link_args = ['-arch', 'arm64', '-isysroot', '$IOS_SDK', '-miphoneos-version-min=$IOS_MIN_VERSION']

[host_machine]
system = 'darwin'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
EOF

    meson setup build-ios \
        --cross-file=ios-cross.txt \
        --prefix="$PREFIX" \
        --default-library=static \
        -Dtests=false \
        -Dintrospection=disabled \
        -Dnls=disabled \
        -Dlibmount=disabled \
        -Dglib_debug=disabled \
        -Dlibelf=disabled \
        > "$LOGDIR/glib-configure.log" 2>&1

    ninja -C build-ios > "$LOGDIR/glib-build.log" 2>&1
    ninja -C build-ios install > "$LOGDIR/glib-install.log" 2>&1
    echo "glib done"
fi

# Build QEMU
echo "Building QEMU..."
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

unset CFLAGS CXXFLAGS LDFLAGS
# Force use of internal slirp by hiding system pkg-config paths
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"

"$QEMUSRC/configure" \
  --cross-prefix="" \
  --cc="$CC" \
  --cxx="$CXX" \
  --objcc="$CC" \
  --cpu=aarch64 \
  --target-list=x86_64-softmmu,riscv64-softmmu \
  --without-default-features \
  --enable-tcg \
  --enable-slirp \
  --enable-virtfs \
  --disable-werror \
  --disable-docs \
  --disable-guest-agent \
  --disable-tools \
  --extra-cflags="$IOS_FLAGS -I$PREFIX/include -I$PREFIX/include/glib-2.0 -I$PREFIX/lib/glib-2.0/include" \
  --extra-cxxflags="$IOS_FLAGS -I$PREFIX/include -I$PREFIX/include/glib-2.0 -I$PREFIX/lib/glib-2.0/include" \
  --extra-objcflags="$IOS_FLAGS" \
  --extra-ldflags="$IOS_FLAGS -L$PREFIX/lib" \
  > "$LOGDIR/configure-ios.log" 2>&1

echo "Configure done. Building..."

make -j$NCPU > "$LOGDIR/build-ios.log" 2>&1

echo "Build complete. Binary at: $BUILDDIR/qemu-system-x86_64"
