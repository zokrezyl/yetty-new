# QEMU platform: ios-x86_64 (iOS Simulator on Intel macs; cross from macOS)

IOS_SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
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
_STRIP_BIN="strip"
