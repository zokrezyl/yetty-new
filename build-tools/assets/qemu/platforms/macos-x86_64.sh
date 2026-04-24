# QEMU platform: macos-x86_64 (native on macOS runner)

_CONFIGURE_ARGS+=(
    --cc=clang
    --cxx=clang++
    --extra-cflags="-arch x86_64"
    --extra-cxxflags="-arch x86_64"
)
_EXTRA_LDFLAGS="-Wl,-dead_strip -arch x86_64"
_QEMU_BINARY_NAME="qemu-system-riscv64-unsigned"
_QEMU_OUTPUT_NAME="qemu-system-riscv64"
_STRIP_BIN="strip"
