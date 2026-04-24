# QEMU platform: android-arm64-v8a (Android NDK direct; no pkgsCross).
# glib/pixman/pcre2/libffi are built from source into a per-ABI sysroot —
# see android-common.sh. NDK clang comes from the .#assets-qemu-android-*
# nix shell.

ANDROID_TRIPLE="aarch64-linux-android"
ANDROID_CPU="aarch64"

# shellcheck source=android-common.sh
source "$(dirname "${BASH_SOURCE[0]}")/android-common.sh"
