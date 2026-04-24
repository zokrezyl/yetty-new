#!/usr/bin/env bash
set -e

SRCDIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SRCDIR/../../.." && pwd)"

# Re-exec inside the purpose-built devShell (flake.nix:368) with a clean env,
# so any nix-clang inherited from an outer devShell is dropped from PATH and
# /usr/bin/clang (host/Apple SDK-aware) is used — matching the shell's own
# comment: "macos native uses the host's clang + nix-provided glib/pixman".
if [ -z "$YETTY_QEMU_MACOS_SHELL" ]; then
    export YETTY_QEMU_MACOS_SHELL=1
    exec nix develop --ignore-environment \
        --keep HOME --keep USER --keep TERM --keep YETTY_QEMU_MACOS_SHELL \
        "$REPO_ROOT#assets-qemu-macos-x86_64" \
        --command bash "$0" "$@"
fi

# Append Apple system paths so /usr/bin/clang, /usr/sbin/sysctl, /usr/bin/ar
# etc. are discoverable — --ignore-environment strips them otherwise.
export PATH="$PATH:/usr/bin:/usr/sbin:/bin:/sbin"

QEMU_VERSION="11.0.0-rc4"
QEMU_TARBALL="qemu-${QEMU_VERSION}.tar.xz"
QEMU_URL="https://download.qemu.org/${QEMU_TARBALL}"

CACHEDIR="$SRCDIR/.cache"
QEMUSRC="$SRCDIR/qemu-${QEMU_VERSION}"
BUILDDIR="$SRCDIR/build-macos-minimal"
LOGDIR="$SRCDIR/tmp"
OUTDIR="$REPO_ROOT/build-tools/assets/qemu"

NCPU=$(sysctl -n hw.ncpu)

# Force Apple's ar/ranlib to avoid GNU thin archive format
export AR=/usr/bin/ar
export RANLIB=/usr/bin/ranlib

mkdir -p "$LOGDIR" "$CACHEDIR" "$OUTDIR"

# Download tarball if missing
if [ ! -f "$CACHEDIR/$QEMU_TARBALL" ]; then
    echo "Downloading $QEMU_URL ..."
    curl -L --fail -o "$CACHEDIR/$QEMU_TARBALL.part" "$QEMU_URL"
    mv "$CACHEDIR/$QEMU_TARBALL.part" "$CACHEDIR/$QEMU_TARBALL"
fi

# Extract if source tree not already present
if [ ! -f "$QEMUSRC/configure" ]; then
    echo "Extracting $QEMU_TARBALL ..."
    tar xJf "$CACHEDIR/$QEMU_TARBALL" -C "$SRCDIR"
fi

rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# Use minimal device config (shared with cmake/linux build)
mkdir -p "$QEMUSRC/configs/devices/riscv64-softmmu"
cp "$SRCDIR/../configs/riscv64-softmmu/default.mak" "$QEMUSRC/configs/devices/riscv64-softmmu/default.mak"

SYSTEM_CC="/usr/bin/gcc"
SYSTEM_CXX="/usr/bin/g++"

"$QEMUSRC/configure" \
  --target-list=riscv64-softmmu \
  --without-default-features \
  --enable-tcg \
  --enable-slirp \
  --enable-virtfs \
  --enable-fdt=internal \
  --enable-trace-backends=nop \
  --disable-werror \
  --disable-docs \
  --disable-guest-agent \
  --disable-tools \
  --disable-qom-cast-debug \
  --disable-coroutine-pool \
  --cc="$SYSTEM_CC" \
  --cxx="$SYSTEM_CXX" \
  --extra-cflags="-Os -ffunction-sections -fdata-sections" \
  --extra-cxxflags="-Os -ffunction-sections -fdata-sections" \
  --extra-ldflags="-Wl,-dead_strip" \
  > "$LOGDIR/configure-macos-minimal.log" 2>&1

echo "Configure done. Building..."
make -j$NCPU > "$LOGDIR/build-macos-minimal.log" 2>&1

echo "Build complete. Installing to $OUTDIR ..."
cp "$BUILDDIR/qemu-system-riscv64" "$OUTDIR/qemu-system-riscv64"
ls -la "$OUTDIR/qemu-system-riscv64"
