# QEMU platform: linux-x86_64 (native on Linux host)
# sourced from _build.sh — sets _CONFIGURE_ARGS / _QEMU_BINARY_NAME / _STRIP_BIN

_CONFIGURE_ARGS+=(
    --cc=gcc
    --cxx=g++
    --enable-virtfs
    --enable-attr
)
