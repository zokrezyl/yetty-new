# QEMU platform: ios-x86_64 (iOS Simulator on Intel macs; cross from macOS)

# Use Apple's xcrun directly — nix's assets-qemu-ios-x86_64 shell puts
# xcbuild's stub `xcrun` on PATH, which doesn't know iphonesimulator SDKs.
IOS_SDK="$(/usr/bin/xcrun --sdk iphonesimulator --show-sdk-path)"
IOS_MIN_VERSION="${IOS_MIN_VERSION:-15.0}"

_IOS_CFLAGS="-isysroot $IOS_SDK -arch x86_64 -mios-simulator-version-min=$IOS_MIN_VERSION"

_CONFIGURE_ARGS+=(
    --cc=clang
    --cxx=clang++
    --host-cc=clang
    --extra-cflags="$_IOS_CFLAGS"
    --extra-cxxflags="$_IOS_CFLAGS"
)
_EXTRA_LDFLAGS="-Wl,-dead_strip $_IOS_CFLAGS"
_QEMU_BINARY_NAME="qemu-system-riscv64-unsigned"
_QEMU_OUTPUT_NAME="qemu-system-riscv64"
_STRIP_BIN="strip"
