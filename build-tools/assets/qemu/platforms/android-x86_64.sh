# QEMU platform: android-x86_64 (custom pkgsX86_64Android crossSystem)
#
# Shell provides a nix-cross clang with prefix
# x86_64-unknown-linux-android-, plus glib/pixman/zlib cross-built for
# x86_64 Android bionic. Produces a binary for the emulator.

: "${CROSS_PREFIX:=x86_64-unknown-linux-android-}"

_CONFIGURE_ARGS+=(
    --cross-prefix="$CROSS_PREFIX"
    --cc="${CROSS_PREFIX}clang"
    --cxx="${CROSS_PREFIX}clang++"
    --host-cc=gcc
)
_STRIP_BIN="${CROSS_PREFIX}strip"
