#!/usr/bin/env bash
# libco 3rdparty wrapper. Note: emscripten is intentionally absent from
# the platform list — yetty's webasm coroutine backend uses
# emscripten_fiber_t (Asyncify) directly, see ycoroutine.c on that
# platform. Same exclusion the from-source libs/co.cmake had.
#
# Required env:
#   TARGET_PLATFORM   linux-x86_64 | linux-aarch64 |
#                     macos-arm64 | macos-x86_64 |
#                     android-arm64-v8a | android-x86_64 |
#                     ios-arm64 | ios-x86_64 |
#                     windows-x86_64
#   OUTPUT_DIR        where the tarball is written

set -euo pipefail

: "${TARGET_PLATFORM:?TARGET_PLATFORM is required}"

case "$TARGET_PLATFORM" in
    linux-x86_64|linux-aarch64|\
    macos-x86_64|macos-arm64|\
    android-arm64-v8a|android-x86_64|\
    ios-arm64|ios-x86_64)
        SHELL_NAME="3rdparty-${TARGET_PLATFORM}"
        ;;
    windows-x86_64)
        if [ "${MSYSTEM:-}" != "CLANG64" ]; then
            echo "error: windows-x86_64 must run inside MSYS2 CLANG64 (MSYSTEM=${MSYSTEM:-unset})" >&2
            exit 1
        fi
        exec bash "$(dirname "$0")/_build.sh" "$@"
        ;;
    webasm)
        echo "libco does not target webasm — yetty's webasm coroutines use" >&2
        echo "emscripten_fiber_t (Asyncify) instead. Skipping." >&2
        exit 1
        ;;
    *)
        echo "unknown TARGET_PLATFORM: $TARGET_PLATFORM" >&2
        exit 1
        ;;
esac

if [ "${USE_NIX:-1}" = "0" ]; then
    exec bash "$(dirname "$0")/_build.sh" "$@"
fi

cd "$(dirname "$0")/../../.."
exec nix develop ".#$SHELL_NAME" --command bash build-tools/3rdparty/libco/_build.sh "$@"
