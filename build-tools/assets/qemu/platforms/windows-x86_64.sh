# QEMU platform: windows-x86_64 (mingw-w64 cross from Linux)
# Toolchain comes from pkgsCross.mingwW64 in nix shell.

: "${CROSS_PREFIX:=x86_64-w64-mingw32-}"

_CONFIGURE_ARGS+=(
    --cross-prefix="$CROSS_PREFIX"
    --cc="${CROSS_PREFIX}gcc"
    --cxx="${CROSS_PREFIX}g++"
    --host-cc=gcc
)
_QEMU_BINARY_NAME="qemu-system-riscv64.exe"
_STRIP_BIN="${CROSS_PREFIX}strip"
