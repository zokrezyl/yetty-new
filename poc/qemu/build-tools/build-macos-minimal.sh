#!/bin/bash
set -e

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
BUILDDIR="$SRCDIR/build-macos-minimal"
LOGDIR="$SRCDIR/tmp"
QEMUSRC="$SRCDIR/qemu-11.0.0-rc4"

NCPU=$(sysctl -n hw.ncpu)

# Force Apple's ar/ranlib to avoid GNU thin archive format
export AR=/usr/bin/ar
export RANLIB=/usr/bin/ranlib

mkdir -p "$LOGDIR"
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# Use minimal device config
cp -r "$SRCDIR/configs-minimal/riscv64-softmmu" "$QEMUSRC/configs/devices/"

"$QEMUSRC/configure" \
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
  --extra-cflags="-Os" \
  --extra-cxxflags="-Os" \
  > "$LOGDIR/configure-macos-minimal.log" 2>&1

echo "Configure done. Building..."
make -j$NCPU > "$LOGDIR/build-macos-minimal.log" 2>&1

echo "Build complete:"
ls -la "$BUILDDIR"/qemu-system-riscv64*
