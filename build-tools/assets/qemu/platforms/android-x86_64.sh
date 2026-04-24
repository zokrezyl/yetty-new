# QEMU platform: android-x86_64 (Android NDK direct; no pkgsCross).
# glib/pixman/pcre2/libffi are built from source into a per-ABI sysroot —
# see android-common.sh. NDK clang comes from the .#assets-qemu-android-*
# nix shell.

ANDROID_TRIPLE="x86_64-linux-android"
ANDROID_CPU="x86_64"

# shellcheck source=android-common.sh
source "$(dirname "${BASH_SOURCE[0]}")/android-common.sh"
