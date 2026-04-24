# QEMU platform: macos-arm64 (native on macOS runner; no nix wrapping)
# CI installs the prereqs: brew install ninja meson pkg-config glib pixman.

_CONFIGURE_ARGS+=(
    --cc=clang
    --cxx=clang++
    --extra-cflags="-arch arm64"
    --extra-cxxflags="-arch arm64"
)
_EXTRA_LDFLAGS="-Wl,-dead_strip -arch arm64"
_QEMU_BINARY_NAME="qemu-system-riscv64-unsigned"
_QEMU_OUTPUT_NAME="qemu-system-riscv64"
_STRIP_BIN="strip"
