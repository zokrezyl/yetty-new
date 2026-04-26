#!/usr/bin/env bash
# openh264 3rdparty wrapper: picks the right nix shell for $TARGET_PLATFORM
# and re-execs into _build.sh inside it.
#
# Required env:
#   TARGET_PLATFORM   one of:
#     linux-x86_64 | linux-aarch64 |
#     macos-arm64 | macos-x86_64 |
#     android-arm64-v8a | android-x86_64 |
#     ios-arm64 | ios-x86_64 |
#     webasm | windows-x86_64
#   OUTPUT_DIR        where the tarball is written
#
# Version is read from this directory's `version` file — single source of
# truth for both upstream fetch and tarball naming. CI tag pattern:
# `lib-openh264-<version>` → tarball `openh264-<target>-<version>.tar.gz`.
# Set USE_NIX=0 to dispatch _build.sh without nix develop (CI / manual env).

set -euo pipefail

: "${TARGET_PLATFORM:?TARGET_PLATFORM is required}"

case "$TARGET_PLATFORM" in
    linux-x86_64|linux-aarch64|\
    macos-x86_64|macos-arm64|\
    android-arm64-v8a|android-x86_64|\
    ios-arm64|ios-x86_64|\
    webasm)
        SHELL_NAME="3rdparty-${TARGET_PLATFORM}"
        ;;
    windows-x86_64)
        # Windows uses MSYS2 CLANG64 — no nix shell. CI: msys2/setup-msys2
        # with msystem: CLANG64; locally: any MSYS2 shell with
        # `MSYSTEM=CLANG64 bash -lc ...`.
        if [ "${MSYSTEM:-}" != "CLANG64" ]; then
            echo "error: windows-x86_64 must run inside MSYS2 CLANG64 (MSYSTEM=${MSYSTEM:-unset})" >&2
            exit 1
        fi
        exec bash "$(dirname "$0")/_build.sh" "$@"
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
exec nix develop ".#$SHELL_NAME" --command bash build-tools/3rdparty/openh264/_build.sh "$@"
