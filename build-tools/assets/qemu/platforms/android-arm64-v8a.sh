# QEMU platform: android-arm64-v8a (nixpkgs pkgsCross.aarch64-android)
#
# The shell sets $CC etc. to aarch64-unknown-linux-android-clang. The
# resulting binary targets Android's bionic libc (not glibc) and
# should run on Android 10+ (API 29) by default for this nix target.

: "${CROSS_PREFIX:=aarch64-unknown-linux-android-}"

_CONFIGURE_ARGS+=(
    --cross-prefix="$CROSS_PREFIX"
    --cc="${CROSS_PREFIX}clang"
    --cxx="${CROSS_PREFIX}clang++"
    --host-cc=gcc
)
_STRIP_BIN="${CROSS_PREFIX}strip"
