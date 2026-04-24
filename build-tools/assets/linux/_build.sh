#!/bin/bash
# Build Linux kernel for RISC-V64 + bundle Alpine minirootfs, producing
# linux-${VERSION}.tar.gz containing:
#   kernel-riscv64.bin
#   alpine-rootfs/...  (with /init pre-written for console boot)
#
# Env vars:
#   VERSION         required — e.g. 0.0.1; used in output filename
#   OUTPUT_DIR      required — where to place the tarball
#   REPO_ROOT       optional — yetty checkout root (default: ../../.. from this script)
#   WORK_DIR        optional — intermediate build tree (default: /tmp/yetty-asset-linux)
#   LINUX_VERSION   optional — kernel tag (default: 7.0)
#   ALPINE_VERSION  optional — alpine minor (default: 3.21)
#   ALPINE_RELEASE  optional — alpine full (default: 3.21.7)
#   CROSS_COMPILE   optional — toolchain prefix (default: riscv64-unknown-linux-gnu-)
#
# Hermetic Linux build: needs make, RISC-V cross-toolchain, bc, bison,
# flex, libssl-dev, libelf-dev, cpio, rsync, curl, tar.

set -euo pipefail

: "${VERSION:?VERSION is required}"
: "${OUTPUT_DIR:?OUTPUT_DIR is required}"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="${REPO_ROOT:-$(cd "$SCRIPT_DIR/../../.." && pwd)}"
WORK_DIR="${WORK_DIR:-/tmp/yetty-asset-linux}"
LINUX_VERSION="${LINUX_VERSION:-7.0}"
ALPINE_VERSION="${ALPINE_VERSION:-3.21}"
ALPINE_RELEASE="${ALPINE_RELEASE:-3.21.7}"
CROSS_COMPILE="${CROSS_COMPILE:-riscv64-unknown-linux-gnu-}"

KERNEL_CONFIG="$REPO_ROOT/poc/qemu/configs/linux-kernel-${LINUX_VERSION}.config"
[ -f "$KERNEL_CONFIG" ] || { echo "missing kernel config: $KERNEL_CONFIG" >&2; exit 1; }

NCPU="$(nproc 2>/dev/null || echo 4)"

KERNEL_URL="https://github.com/torvalds/linux/archive/refs/tags/v${LINUX_VERSION}.tar.gz"
ALPINE_URL="https://dl-cdn.alpinelinux.org/alpine/v${ALPINE_VERSION}/releases/riscv64/alpine-minirootfs-${ALPINE_RELEASE}-riscv64.tar.gz"

mkdir -p "$WORK_DIR" "$OUTPUT_DIR"
cd "$WORK_DIR"

#-----------------------------------------------------------------------------
# Kernel
#-----------------------------------------------------------------------------

if [ ! -f "linux-${LINUX_VERSION}.tar.gz" ]; then
    echo "==> downloading Linux ${LINUX_VERSION}"
    curl -fL --retry 3 -o "linux-${LINUX_VERSION}.tar.gz" "$KERNEL_URL"
fi

if [ ! -d "linux-${LINUX_VERSION}" ]; then
    echo "==> extracting kernel"
    tar xf "linux-${LINUX_VERSION}.tar.gz"
fi

cp "$KERNEL_CONFIG" "linux-${LINUX_VERSION}/.config"

echo "==> building kernel (CROSS_COMPILE=${CROSS_COMPILE}, -j${NCPU})"
make -C "linux-${LINUX_VERSION}" \
    ARCH=riscv CROSS_COMPILE="$CROSS_COMPILE" \
    -j"$NCPU" olddefconfig Image

KERNEL_IMAGE="linux-${LINUX_VERSION}/arch/riscv/boot/Image"
[ -f "$KERNEL_IMAGE" ] || { echo "missing $KERNEL_IMAGE" >&2; exit 1; }

#-----------------------------------------------------------------------------
# Alpine minirootfs
#-----------------------------------------------------------------------------

if [ ! -f "alpine-minirootfs-${ALPINE_RELEASE}-riscv64.tar.gz" ]; then
    echo "==> downloading Alpine ${ALPINE_RELEASE} minirootfs"
    curl -fL --retry 3 -o "alpine-minirootfs-${ALPINE_RELEASE}-riscv64.tar.gz" "$ALPINE_URL"
fi

ROOTFS_DIR="$WORK_DIR/alpine-rootfs"
if [ ! -f "$ROOTFS_DIR/bin/busybox" ]; then
    echo "==> extracting Alpine rootfs"
    rm -rf "$ROOTFS_DIR"
    mkdir -p "$ROOTFS_DIR"
    tar -C "$ROOTFS_DIR" -xzf "alpine-minirootfs-${ALPINE_RELEASE}-riscv64.tar.gz"
fi

# /init for TinyEMU / QEMU console boot (slirp: 10.0.2.x user-mode net)
cat > "$ROOTFS_DIR/init" << 'INIT_EOF'
#!/bin/sh
mount -t proc proc /proc
mount -t sysfs sys /sys
mount -t devtmpfs dev /dev 2>/dev/null || true
hostname tinyemu

ip link set lo up
ip link set eth0 up 2>/dev/null
ip addr add 10.0.2.15/24 dev eth0 2>/dev/null
ip route add default via 10.0.2.2 2>/dev/null
echo "nameserver 10.0.2.3" > /etc/resolv.conf

exec /bin/sh
INIT_EOF
chmod 755 "$ROOTFS_DIR/init"

#-----------------------------------------------------------------------------
# Stage + package
#-----------------------------------------------------------------------------

STAGE="$WORK_DIR/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE"

cp "$KERNEL_IMAGE" "$STAGE/kernel-riscv64.bin"
# rsync preserves perms/symlinks; skip /dev so device nodes don't leak
rsync -a --delete --exclude='dev/*' "$ROOTFS_DIR/" "$STAGE/alpine-rootfs/"
mkdir -p "$STAGE/alpine-rootfs/dev"

TARBALL="$OUTPUT_DIR/linux-${VERSION}.tar.gz"
echo "==> packaging -> $TARBALL"
tar -C "$STAGE" -czf "$TARBALL" .

echo ""
echo "Linux asset ready:"
ls -lh "$TARBALL"
ENTRIES="$(tar -tzf "$TARBALL" | wc -l)"
echo "contents (first 20 of $ENTRIES):"
tar -tzf "$TARBALL" | sed -n '1,20p'
