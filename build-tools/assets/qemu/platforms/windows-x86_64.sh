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

# QEMU's meson.build line ~334 whitelists only 'gcc' / 'clang' /
# 'emscripten' compiler IDs, but clang-cl identifies as 'clang-cl' and
# falls through to the "You either need GCC ..." error. Extend the
# clang branch to also accept clang-cl — it's still Clang, just with
# MSVC-driver personality.
if grep -q "compiler.get_id() == 'clang' and compiler.compiles" "$SRC_DIR/meson.build"; then
    sed -i "s|compiler.get_id() == 'clang' and compiler.compiles|(compiler.get_id() == 'clang' or compiler.get_id() == 'clang-cl') and compiler.compiles|" \
        "$SRC_DIR/meson.build"
    echo "==> patched $SRC_DIR/meson.build: clang-cl accepted as Clang"
fi

# QEMU's configure sets CPU_CFLAGS="-m64" for x86_64/s390x hosts — a
# GCC/Clang-driver flag. clang-cl (MSVC driver) rejects it; target arch
# is already picked by vcvarsall's target triple. Drop the flag; we
# only build for x86_64 Windows on this platform anyway.
if grep -q 'CPU_CFLAGS="-m64"' "$SRC_DIR/configure"; then
    sed -i 's|CPU_CFLAGS="-m64"|CPU_CFLAGS=""|g' "$SRC_DIR/configure"
    echo "==> patched $SRC_DIR/configure: dropped -m64 for clang-cl"
fi

# Point pkg-config at the vcpkg tree
export PKG_CONFIG_PATH="$VCPKG_INSTALLED/lib/pkgconfig"

# Meson needs a real Windows-native pkgconf.exe here. Strawberry Perl's
# pkg-config is a Perl-script wrapper that Meson can't execute through
# Windows' CreateProcess. Also: the path must be Windows-style
# (`C:\...`), not POSIX-style (`/c/...`), because Meson hands it to
# subprocess exec.
if [ -z "${PKG_CONFIG:-}" ]; then
    # Prefer the msys2 pkgconf.exe that vcpkg bootstraps. Fall back to
    # any pkgconf on PATH (not pkg-config; that's Strawberry's script).
    # Normalise VCPKG_ROOT to a POSIX path bash can glob.
    _VCPKG_ROOT_FS="$(cygpath -u "$VCPKG_ROOT" 2>/dev/null || echo "$VCPKG_ROOT")"
    echo "==> searching pkgconf under $_VCPKG_ROOT_FS/downloads/tools/msys2"
    _PKGCONF_CANDIDATES=()
    while IFS= read -r -d '' _p; do
        _PKGCONF_CANDIDATES+=("$_p")
    done < <(find "$_VCPKG_ROOT_FS/downloads/tools/msys2" -name pkgconf.exe -print0 2>/dev/null)
    for _c in "${_PKGCONF_CANDIDATES[@]}"; do
        if [ -x "$_c" ]; then PKG_CONFIG="$_c"; break; fi
    done
    [ -z "${PKG_CONFIG:-}" ] && PKG_CONFIG="$(command -v pkgconf 2>/dev/null || true)"
    if [ -z "${PKG_CONFIG:-}" ]; then
        echo "error: pkgconf.exe not found (checked vcpkg msys2 tools + PATH)" >&2
        exit 1
    fi
fi
# If the found path is POSIX-style (/c/...), convert to Windows (C:\...).
case "$PKG_CONFIG" in
    /[a-z]/*)
        _drive="${PKG_CONFIG:1:1}"
        _rest="${PKG_CONFIG:3}"
        PKG_CONFIG="${_drive^^}:/${_rest}"
        ;;
esac
export PKG_CONFIG
echo "==> PKG_CONFIG=$PKG_CONFIG"

# MSVC flags:
#   /O2 /Oi /Gy /Gw = release-level optimisation, function/data COMDAT
#     folding (equivalent intent to -Os -ffunction-sections -fdata-sections).
#   /I and /LIBPATH wire in the vcpkg tree.
_EXTRA_CFLAGS="/O2 /Oi /Gy /Gw /I$VCPKG_INSTALLED/include"
_EXTRA_CXXFLAGS="$_EXTRA_CFLAGS"
# /OPT:REF /OPT:ICF are the MSVC linker equivalents of --gc-sections.
_EXTRA_LDFLAGS="/OPT:REF /OPT:ICF /LIBPATH:$VCPKG_INSTALLED/lib"

# meson's cc.find_library (used e.g. for pathcch, synchronization, ws2_32)
# doesn't consult $LIB the way cl.exe's link step does. Convert every
# LIB entry into an explicit /LIBPATH: so the Windows SDK + MSVC libs
# are discoverable by meson itself.
if [ -n "${LIB:-}" ]; then
    IFS=';' read -ra _LIB_ENTRIES <<< "$LIB"
    for _p in "${_LIB_ENTRIES[@]}"; do
        [ -n "$_p" ] && _EXTRA_LDFLAGS="$_EXTRA_LDFLAGS /LIBPATH:$_p"
    done
fi

# Disable the --static QEMU configure flag (or rather: its b_static_pie
# side-effect) on Windows; the SDK ships pathcch/shlwapi only as import
# libs to DLLs, so static-only resolution fails.
_STATIC=no

_CONFIGURE_ARGS+=(
    # QEMU's meson.build:348 requires GCC or Clang — cl.exe is rejected.
    # clang-cl is Clang's MSVC-compatible driver: accepts MSVC flags,
    # uses MSVC's linker, produces MSVC-ABI .exe, but the compiler
    # frontend is Clang so meson accepts it.
    --cc=clang-cl
    --cxx=clang-cl
    --host-cc=clang-cl
)

_QEMU_BINARY_NAME="qemu-system-riscv64.exe"
_STRIP_BIN=":"   # MSVC produces stripped release binaries; no separate strip
