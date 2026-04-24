# QEMU platform: ios-arm64 (iPhone/iPad device; cross from macOS)
#
# Uses the iPhoneOS SDK from Xcode. Based on poc/qemu/build-tools/build-ios.sh.
# Run this from a macOS runner; no nix involvement.

# Use Apple's xcrun directly — nix's assets-qemu-ios-arm64 shell puts
# xcbuild's stub `xcrun` on PATH, which doesn't know iphoneos SDKs.
IOS_SDK="$(/usr/bin/xcrun --sdk iphoneos --show-sdk-path)"
IOS_MIN_VERSION="${IOS_MIN_VERSION:-15.0}"

_IOS_CFLAGS="-isysroot $IOS_SDK -arch arm64 -mios-version-min=$IOS_MIN_VERSION"

_CONFIGURE_ARGS+=(
    --cc=clang
    --cxx=clang++
    --host-cc=clang
    --cross-prefix=""
    --extra-cflags="$_IOS_CFLAGS"
    --extra-cxxflags="$_IOS_CFLAGS"
)
_EXTRA_LDFLAGS="-Wl,-dead_strip $_IOS_CFLAGS"
_QEMU_BINARY_NAME="qemu-system-riscv64-unsigned"
_QEMU_OUTPUT_NAME="qemu-system-riscv64"
_STRIP_BIN="strip"
