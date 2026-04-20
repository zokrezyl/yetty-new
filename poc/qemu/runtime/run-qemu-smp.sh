#!/bin/bash
# Run QEMU with multi-threaded TCG for RISC-V SMP testing

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IOS_QEMU_ROOT="$(dirname "$SCRIPT_DIR")"

QEMU="$IOS_QEMU_ROOT/build-ios-sim/qemu-system-riscv64-unsigned"
KERNEL="$SCRIPT_DIR/kernel-riscv64.bin"
ROOTFS="$IOS_QEMU_ROOT/alpine-fs"

SIM_UDID="F3DD98AB-F5CE-427A-B69F-4E3FB3C8F260"
SMP=${1:-4}

exec xcrun simctl spawn "$SIM_UDID" "$QEMU" \
  -machine virt \
  -smp "$SMP" \
  -m 256 \
  -bios default \
  -kernel "$KERNEL" \
  -append "earlycon=sbi console=hvc0 root=/dev/root rootfstype=9p rootflags=trans=virtio rw init=/init" \
  -fsdev local,id=fsdev0,path="$ROOTFS",security_model=none \
  -device virtio-9p-device,fsdev=fsdev0,mount_tag=/dev/root \
  -netdev user,id=net0 \
  -device virtio-net-device,netdev=net0 \
  -device virtio-serial-device \
  -device virtconsole,chardev=char0 \
  -chardev stdio,id=char0,signal=off \
  -serial none \
  -display none
