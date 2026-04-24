# QEMU platform: linux-aarch64 (cross-compile from x86_64 Linux)
# Toolchain prefix comes from pkgsCross.aarch64-multiplatform in nix shell.
#
# QEMU configure only accepts --cross-prefix / --cc / --cxx / --host-cc.
# AR/STRIP/RANLIB/NM/OBJCOPY are derived from $AR etc. or ${cross_prefix}ar
# automatically, so plain --cross-prefix is enough here.

: "${CROSS_PREFIX:=aarch64-unknown-linux-gnu-}"

_CONFIGURE_ARGS+=(
    --cross-prefix="$CROSS_PREFIX"
    --cc="${CROSS_PREFIX}gcc"
    --cxx="${CROSS_PREFIX}g++"
    --host-cc=gcc
    --enable-virtfs
    --enable-attr
)
_STRIP_BIN="${CROSS_PREFIX}strip"
