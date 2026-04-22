#!/bin/bash
set -e

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$SRCDIR/build-ios-sim-interp"
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

# Dependencies already built by build-ios-sim.sh, skip if present

# Build QEMU with TCG interpreter
echo "Building QEMU with TCG interpreter..."
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
  --enable-tcg-interpreter \
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
  > "$LOGDIR/configure-ios-sim-interp.log" 2>&1

echo "Configure done. Building..."

make -j$NCPU > "$LOGDIR/build-ios-sim-interp.log" 2>&1

# Rename binaries with -interp suffix
mv "$BUILDDIR/qemu-system-x86_64-unsigned" "$BUILDDIR/qemu-system-x86_64-interp" 2>/dev/null || true
mv "$BUILDDIR/qemu-system-riscv64-unsigned" "$BUILDDIR/qemu-system-riscv64-interp" 2>/dev/null || true

echo "Build complete:"
ls -la "$BUILDDIR"/qemu-system-*-interp
