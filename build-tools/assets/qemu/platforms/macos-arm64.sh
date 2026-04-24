# QEMU platform: macos-arm64 (native on macOS runner; no nix wrapping)
# CI installs the prereqs: brew install ninja meson pkg-config glib pixman.

_CONFIGURE_ARGS+=(
    --cc=clang
    --cxx=clang++
    --extra-cflags="-arch arm64"
    --extra-cxxflags="-arch arm64"
)
_EXTRA_LDFLAGS="-Wl,-dead_strip -arch arm64"
_STRIP_BIN="strip"
