#!/bin/bash
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SIM_UDID="F3DD98AB-F5CE-427A-B69F-4E3FB3C8F260"
QEMU="$SCRIPT_DIR/build-ios-sim-interp/qemu-system-riscv64-interp"
KERNEL="$SCRIPT_DIR/linux-kernel/kernel-riscv64.bin"
ROOTFS="$SCRIPT_DIR/alpine-fs"

xcrun simctl boot "$SIM_UDID" 2>/dev/null || true

exec xcrun simctl spawn "$SIM_UDID" "$QEMU" \
  -machine virt -smp 4 -m 256 -bios default \
  -kernel "$KERNEL" \
  -append "earlycon=sbi console=hvc0 root=/dev/root rootfstype=9p rootflags=trans=virtio rw init=/bench" \
  -fsdev local,id=fsdev0,path="$ROOTFS",security_model=none \
  -device virtio-9p-device,fsdev=fsdev0,mount_tag=/dev/root \
  -netdev user,id=net0 -device virtio-net-device,netdev=net0 \
  -device virtio-serial-device -device virtconsole,chardev=char0 \
  -chardev stdio,id=char0,signal=off -serial none -display none
