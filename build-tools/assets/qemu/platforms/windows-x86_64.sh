# QEMU platform: windows-x86_64 (native MSVC via vcpkg)
#
# Expects to be sourced by _build.sh running under Git Bash on a Windows
# host where:
#   - vcvarsall x64 has already been applied (cl.exe + link.exe on PATH)
#   - meson.exe on PATH
#   - GnuWin32 bin (bison/flex/make) on PATH
#   - vcpkg x64-windows headers/libs at $VCPKG_INSTALLED
#   - pkgconf.exe on PATH (or PKG_CONFIG points at it)
# The wrapper `build.ps1` sets all of this up before invoking build.sh.
#
# QEMU 10.1+ can be built with MSVC for Windows targets. We use cl.exe
# directly; QEMU's configure generates the meson cross file with MSVC
# quirks handled.

: "${VCPKG_INSTALLED:?VCPKG_INSTALLED is required (e.g. C:/Users/misi/vcpkg/installed/x64-windows)}"

# QEMU's configure assumes a UNIX Python-venv layout (`pyvenv/bin/meson`),
# but Windows Python venvs put the wrappers in `pyvenv/Scripts`. Rewrite
# the one line that locates meson so all downstream `$meson` references
# resolve correctly. Safe because `bin` only appears inside this single
# `pyvenv/bin` path in configure.
if grep -q 'pyvenv/bin' "$SRC_DIR/configure"; then
    sed -i 's|pyvenv/bin|pyvenv/Scripts|g' "$SRC_DIR/configure"
    echo "==> patched $SRC_DIR/configure: pyvenv/bin -> pyvenv/Scripts"
fi

# Point pkg-config at the vcpkg tree
export PKG_CONFIG_PATH="$VCPKG_INSTALLED/lib/pkgconfig"
: "${PKG_CONFIG:=pkgconf}"
export PKG_CONFIG

# MSVC flags:
#   /O2 /Oi /Gy /Gw = release-level optimisation, function/data COMDAT
#     folding (equivalent intent to -Os -ffunction-sections -fdata-sections).
#   /I and /LIBPATH wire in the vcpkg tree.
_EXTRA_CFLAGS="/O2 /Oi /Gy /Gw /I$VCPKG_INSTALLED/include"
_EXTRA_CXXFLAGS="$_EXTRA_CFLAGS"
# /OPT:REF /OPT:ICF are the MSVC linker equivalents of --gc-sections.
_EXTRA_LDFLAGS="/OPT:REF /OPT:ICF /LIBPATH:$VCPKG_INSTALLED/lib"

_CONFIGURE_ARGS+=(
    --cc=cl
    --cxx=cl
    --host-cc=cl
)

_QEMU_BINARY_NAME="qemu-system-riscv64.exe"
_STRIP_BIN=":"   # MSVC produces stripped release binaries; no separate strip
