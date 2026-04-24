# QEMU platform: tvos-arm64 (Apple TV device; cross from macOS)

# Use Apple's xcrun directly — nix's assets-qemu-tvos-arm64 shell puts
# xcbuild's stub `xcrun` on PATH, which doesn't know appletvos SDKs.
TVOS_SDK="$(/usr/bin/xcrun --sdk appletvos --show-sdk-path)"
TVOS_MIN_VERSION="${TVOS_MIN_VERSION:-17.0}"

_TVOS_CFLAGS="-isysroot $TVOS_SDK -arch arm64 -mtvos-version-min=$TVOS_MIN_VERSION"

_CONFIGURE_ARGS+=(
    --cc=clang
    --cxx=clang++
    --host-cc=clang
    --extra-cflags="$_TVOS_CFLAGS"
    --extra-cxxflags="$_TVOS_CFLAGS"
)
_EXTRA_LDFLAGS="-Wl,-dead_strip $_TVOS_CFLAGS"
_QEMU_BINARY_NAME="qemu-system-riscv64-unsigned"
_QEMU_OUTPUT_NAME="qemu-system-riscv64"
_STRIP_BIN="strip"
