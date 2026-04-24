# QEMU platform: macos-x86_64 (native on macOS runner)

_CONFIGURE_ARGS+=(
    --cc=clang
    --cxx=clang++
    --extra-cflags="-arch x86_64"
    --extra-cxxflags="-arch x86_64"
)
_EXTRA_LDFLAGS="-Wl,-dead_strip -arch x86_64"
_STRIP_BIN="strip"
