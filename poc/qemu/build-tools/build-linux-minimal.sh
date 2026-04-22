#!/bin/bash
set -e

# Build directory is in the repository root, not source directory
SCRIPTDIR="$(cd "$(dirname "$0")" && pwd)"
REPOROOT="$(cd "$SCRIPTDIR/../../.." && pwd)"
BUILDDIR="$REPOROOT/build-linux-minimal"
LOGDIR="$REPOROOT/tmp"
QEMUSRC="$SCRIPTDIR/qemu-11.0.0-rc4"

# TinyEMU paths for kernel/bios/rootfs
TINYEMU_BUILD="/home/misi/work/my/ios-tinyemu/build-macos"
TINYEMU_TMP="/home/misi/work/my/ios-tinyemu/tmp"

QEMU_VERSION="11.0.0-rc4"
QEMU_URL="https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz"

NCPU=$(nproc)

# Use system gcc (not nix) to ensure /usr/include is searched
SYSTEM_CC="/usr/bin/gcc"
SYSTEM_CXX="/usr/bin/g++"

# Build acceleration
CC_WRAPPER=""
if [ "$USE_CCACHE" = "1" ]; then
    CC_WRAPPER="ccache"
    echo "Using ccache for build acceleration"
elif [ "$USE_DISTCC" = "1" ]; then
    CC_WRAPPER="distcc"
    echo "Using distcc for build acceleration"
fi

mkdir -p "$LOGDIR"

# Download QEMU source if not present
if [ ! -d "$QEMUSRC" ]; then
    echo "Downloading QEMU $QEMU_VERSION..."
    cd "$SCRIPTDIR"
    if [ ! -f "qemu-${QEMU_VERSION}.tar.xz" ]; then
        wget "$QEMU_URL"
    fi
    echo "Extracting..."
    tar xf "qemu-${QEMU_VERSION}.tar.xz"
fi

# Clean and create build directory
rm -rf "$BUILDDIR"
mkdir -p "$BUILDDIR"
cd "$BUILDDIR"

# Use minimal device config - copy our custom config
mkdir -p "$QEMUSRC/configs/devices/riscv64-softmmu"
cp "$SCRIPTDIR/../configs/riscv64-softmmu/default.mak" "$QEMUSRC/configs/devices/riscv64-softmmu/"

echo "Configuring QEMU..."

# Build configure options
CONFIGURE_OPTS=(
  --target-list=riscv64-softmmu
  --without-default-features
  --enable-tcg
  --enable-slirp
  --enable-virtfs
  --enable-attr
  --enable-fdt=internal
  --enable-trace-backends=nop
  --disable-werror
  --disable-docs
  --disable-guest-agent
  --disable-tools
  --disable-qom-cast-debug
  --disable-coroutine-pool
  --extra-cflags="-Os -ffunction-sections -fdata-sections"
  --extra-cxxflags="-Os -ffunction-sections -fdata-sections"
  --extra-ldflags="-Wl,--gc-sections"
)

if [ -n "$CC_WRAPPER" ]; then
    CONFIGURE_OPTS+=(--cc="$CC_WRAPPER $SYSTEM_CC" --cxx="$CC_WRAPPER $SYSTEM_CXX")
else
    CONFIGURE_OPTS+=(--cc="$SYSTEM_CC" --cxx="$SYSTEM_CXX")
fi

"$QEMUSRC/configure" "${CONFIGURE_OPTS[@]}" > "$LOGDIR/configure-linux-minimal.log" 2>&1

echo "Configure done. Building with $NCPU cores..."
make -j$NCPU > "$LOGDIR/build-linux-minimal.log" 2>&1

echo "Stripping binary..."
strip "$BUILDDIR/qemu-system-riscv64" 2>/dev/null || true

echo ""
echo "Build complete:"
ls -lh "$BUILDDIR/qemu-system-riscv64"

# Copy kernel and rootfs to assets directory
ASSETSDIR="$BUILDDIR/assets"
echo ""
echo "Copying kernel and rootfs to assets..."
mkdir -p "$ASSETSDIR"
cp "$TINYEMU_BUILD/kernel-riscv64.bin" "$ASSETSDIR/kernel-riscv64.bin"
# Use rsync to skip device nodes (not needed for 9p, kernel creates them)
rsync -a --exclude='dev/*' "$TINYEMU_TMP/alpine-rootfs/" "$ASSETSDIR/alpine-rootfs/"
mkdir -p "$ASSETSDIR/alpine-rootfs/dev"

echo ""
echo "Files ready:"
ls -lh "$ASSETSDIR/kernel-riscv64.bin"
du -sh "$ASSETSDIR/alpine-rootfs"

# Create run script (uses -bios default for QEMU's built-in OpenSBI)
cat > "$BUILDDIR/run.sh" << 'RUNEOF'
#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$SCRIPT_DIR"

SMP=${1:-1}

./qemu-system-riscv64 \
  -machine virt \
  -smp "$SMP" \
  -m 256 \
  -bios default \
  -kernel assets/kernel-riscv64.bin \
  -append "earlycon=sbi console=hvc0 root=/dev/root rootfstype=9p rootflags=trans=virtio rw init=/init" \
  -fsdev local,id=fsdev0,path=assets/alpine-rootfs,security_model=none \
  -device virtio-9p-device,fsdev=fsdev0,mount_tag=/dev/root \
  -netdev user,id=net0 \
  -device virtio-net-device,netdev=net0 \
  -device virtio-serial-device \
  -device virtconsole,chardev=char0 \
  -chardev stdio,id=char0,signal=off \
  -serial none \
  -display none
RUNEOF
chmod +x "$BUILDDIR/run.sh"

echo ""
echo "Run with: $BUILDDIR/run.sh [smp_count]"
echo "Exit with: Ctrl-A X"
