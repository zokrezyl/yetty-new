#!/bin/bash
set -e

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$SRCDIR/build-ios-sim-minimal"
LOGDIR="$SRCDIR/tmp"
QEMUSRC="$SRCDIR/qemu-11.0.0-rc4"
DEPSDIR="$SRCDIR/deps-ios-sim"
PREFIX="$DEPSDIR/install"

SIM_SDK="/Applications/Xcode.app/Contents/Developer/Platforms/iPhoneSimulator.platform/Developer/SDKs/iPhoneSimulator18.2.sdk"
IOS_MIN_VERSION="14.0"
TOOLCHAIN="/Applications/Xcode.app/Contents/Developer/Toolchains/XcodeDefault.xctoolchain/usr/bin"

SIM_FLAGS="-arch x86_64 -isysroot $SIM_SDK -mios-simulator-version-min=$IOS_MIN_VERSION"

export CC="$TOOLCHAIN/clang"
export CXX="$TOOLCHAIN/clang++"
export AR="$TOOLCHAIN/ar"
export RANLIB="$TOOLCHAIN/ranlib"
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_LIBDIR="$PREFIX/lib/pkgconfig"

NCPU=$(sysctl -n hw.ncpu)

mkdir -p "$LOGDIR"
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# Use minimal device config - disable unused machines
cp -r "$SRCDIR/configs-minimal/riscv64-softmmu" "$QEMUSRC/configs/devices/"

"$QEMUSRC/configure" \
  --cross-prefix="" \
  --cc="$CC" \
  --cxx="$CXX" \
  --objcc="$CC" \
  --cpu=x86_64 \
  --target-list=riscv64-softmmu \
  --without-default-features \
  --enable-tcg \
  --enable-tcg-interpreter \
  --enable-slirp \
  --enable-virtfs \
  --enable-fdt=internal \
  --disable-werror \
  --disable-docs \
  --disable-guest-agent \
  --disable-tools \
  --extra-cflags="$SIM_FLAGS -I$PREFIX/include -I$PREFIX/include/glib-2.0 -I$PREFIX/lib/glib-2.0/include -Os" \
  --extra-cxxflags="$SIM_FLAGS -I$PREFIX/include -I$PREFIX/include/glib-2.0 -I$PREFIX/lib/glib-2.0/include -Os" \
  --extra-objcflags="$SIM_FLAGS" \
  --extra-ldflags="$SIM_FLAGS -L$PREFIX/lib" \
  > "$LOGDIR/configure-ios-sim-minimal.log" 2>&1

echo "Configure done. Building..."
make -j$NCPU > "$LOGDIR/build-ios-sim-minimal.log" 2>&1

strip "$BUILDDIR/qemu-system-riscv64"* 2>/dev/null || true

echo "Build complete:"
ls -la "$BUILDDIR"/qemu-system-riscv64*
