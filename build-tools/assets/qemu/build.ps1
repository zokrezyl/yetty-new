# Windows QEMU asset wrapper (MSVC + vcpkg).
#
# Callable from PowerShell directly on Windows, or over SSH:
#   ssh misi@winy "cd C:\path\to\repo; powershell -ExecutionPolicy Bypass -File build-tools\assets\qemu\build.ps1"
#
# Loads vcvarsall x64, adds meson/GnuWin32/Git/vcpkg/pkgconf to PATH,
# exports VCPKG_INSTALLED, then hands off to Git Bash running build.sh
# (which for TARGET_PLATFORM=windows-x86_64 runs `_build.sh` natively,
# no nix).
#
# Required env (same contract as other platforms):
#   VERSION, OUTPUT_DIR
#   TARGET_PLATFORM (default: windows-x86_64)
#   WORK_DIR (optional)
#
# Optional env:
#   VCPKG_ROOT       (default: C:\Users\misi\vcpkg)
#   VCPKG_TRIPLET    (default: x64-windows)
#   VS_INSTALL_DIR   (default: first found among yetty build.bat paths)

$ErrorActionPreference = "Stop"

function Require-Env($name) {
    $v = [System.Environment]::GetEnvironmentVariable($name)
    if ([string]::IsNullOrEmpty($v)) {
        throw "environment variable $name is required"
    }
    return $v
}

$Version   = Require-Env "VERSION"
$OutputDir = Require-Env "OUTPUT_DIR"
if (-not $env:TARGET_PLATFORM) { $env:TARGET_PLATFORM = "windows-x86_64" }

# Default vcpkg locations — but only locked in after vcvarsall (which
# clobbers VCPKG_ROOT with its own VS-bundled copy that has no packages
# installed).
if (-not $env:VCPKG_TRIPLET) { $env:VCPKG_TRIPLET = "x64-windows" }
$UserVcpkgRoot = if ($env:VCPKG_ROOT) { $env:VCPKG_ROOT } else { "C:\Users\misi\vcpkg" }

#-----------------------------------------------------------------------------
# Locate + invoke vcvarsall x64, mirroring build-tools/windows/build.bat
#-----------------------------------------------------------------------------
$VcvarsCandidates = @(
    "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
    "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat",
    "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat"
)
$Vcvarsall = $VcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $Vcvarsall) {
    throw "vcvarsall.bat not found (install VS Build Tools 2022 or 18 BuildTools)"
}
Write-Host "==> using $Vcvarsall"

# Run vcvarsall in a cmd subshell and import its env back into this process.
$envDump = cmd /c "`"$Vcvarsall`" x64 >NUL & set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

# vcvarsall's $LIB/$INCLUDE entries live under "C:\Program Files (x86)\..."
# — paths with spaces. meson/clang-cl pass them down to lld-link, which
# splits on whitespace and fails. Rewrite each entry to its Windows 8.3
# short name (PROGRA~2) so every component is space-free.
function To-ShortPath([string]$p) {
    $fso = New-Object -ComObject Scripting.FileSystemObject
    if (Test-Path $p -PathType Container) { return $fso.GetFolder($p).ShortPath }
    if (Test-Path $p)                     { return $fso.GetFile($p).ShortPath }
    return $p
}
function Convert-PathListToShort([string]$list, [string]$sep = ';') {
    if (-not $list) { return $list }
    return (($list -split [regex]::Escape($sep) |
             Where-Object { $_ } |
             ForEach-Object { To-ShortPath $_ }) -join $sep)
}
foreach ($v in @('LIB', 'INCLUDE', 'LIBPATH')) {
    $orig = [System.Environment]::GetEnvironmentVariable($v)
    if ($orig) {
        [System.Environment]::SetEnvironmentVariable($v, (Convert-PathListToShort $orig))
    }
}

# vcvarsall clobbers VCPKG_ROOT with its own (empty) bundled vcpkg —
# restore our user-level vcpkg after.
$env:VCPKG_ROOT = $UserVcpkgRoot
$VcpkgInstalled = Join-Path $env:VCPKG_ROOT "installed\$($env:VCPKG_TRIPLET)"
if (-not (Test-Path $VcpkgInstalled)) {
    throw "vcpkg tree not found at $VcpkgInstalled"
}
$env:VCPKG_INSTALLED = $VcpkgInstalled.Replace('\', '/')

#-----------------------------------------------------------------------------
# Prepend extra tool dirs to PATH so Git Bash inherits them
#-----------------------------------------------------------------------------
$ExtraPath = @(
    "C:\Program Files\LLVM\bin",                # clang-cl (QEMU requires Clang, not cl.exe)
    "C:\Program Files\Meson",                   # meson.exe
    "C:\Program Files (x86)\GnuWin32\bin",      # bison, flex, m4, make
    "C:\Program Files\Git\cmd",                 # git on PATH
    "C:\Program Files\Git\mingw64\bin",         # bzip2, sed, tar (Git-Bash --noprofile skips this)
    "C:\Strawberry\perl\bin",                   # perl for QEMU scripts
    "$VcpkgInstalled\tools\pkgconf"             # pkgconf.exe (if vcpkg pkgconf installed)
)
# Git\usr\bin goes at the END — it has coreutils `link.exe` that would
# otherwise shadow MSVC's linker. Keep it reachable for tools like
# `find`, `grep`, `rm` that bash scripts rely on.
$TailPath = @(
    "C:\Program Files\Git\usr\bin"
)
# vcpkg downloads an msys2 with pkgconf under it; fall back to that.
$Msys2PkgConf = Get-ChildItem -Path "$env:VCPKG_ROOT\downloads\tools\msys2" `
    -Recurse -Filter "pkgconf.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($Msys2PkgConf) { $ExtraPath += (Split-Path $Msys2PkgConf.FullName) }

$env:PATH = ($ExtraPath -join ";") + ";" + $env:PATH + ";" + ($TailPath -join ";")

#-----------------------------------------------------------------------------
# Hand off to Git Bash running build.sh
#
# NOTE: git-bash prepends `/usr/bin` to PATH on startup, which contains a
# GNU coreutils `link.exe` that shadows MSVC's linker. We therefore
# invoke bash with --noprofile --norc and construct PATH ourselves in
# POSIX form, putting the MSVC dirs before /usr/bin.
#-----------------------------------------------------------------------------
$GitBash = @(
    "C:\Program Files\Git\bin\bash.exe",
    "C:\Program Files\Git\usr\bin\bash.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $GitBash) { throw "Git Bash not found" }

function To-PosixPath([string]$p) {
    # C:\Foo\Bar -> /c/Foo/Bar
    $p = $p.TrimEnd('\').Replace('\', '/')
    if ($p -match '^([A-Za-z]):(/.*)?$') { "/$($matches[1].ToLower())$($matches[2])" } else { $p }
}

# Convert the current Windows PATH to a POSIX-style colon-separated list
# so bash picks up MSVC/meson/gnuwin32 in the order we set.
$PosixPath = ($env:PATH.Split(';') | Where-Object { $_ } | ForEach-Object { To-PosixPath $_ }) -join ':'
# Put /usr/bin AFTER the rest so MSVC link.exe wins over GNU coreutils link.
$PosixPath = "$PosixPath`:/usr/bin:/bin"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
$RepoRootPosix = To-PosixPath $RepoRoot.Path

Push-Location $RepoRoot
try {
    # Pass env to bash via an explicit here-string. PowerShell's
    # inline string interpolation of multi-arg command lines was
    # eating single quotes when $env:TARGET_PLATFORM etc. got
    # embedded — causing `TARGET_PLATFORM required` inside bash.
    # Bash -c over the Windows CreateProcess boundary is flaky with
    # multi-line args, so write the script to a temp file and invoke
    # bash on it. That also sidesteps any quoting surprises.
    $TempScript = [System.IO.Path]::GetTempFileName() + ".sh"
    $ScriptBody = @"
set -euo pipefail
export PATH='$PosixPath'
export TARGET_PLATFORM='$env:TARGET_PLATFORM'
export VERSION='$env:VERSION'
export OUTPUT_DIR='$env:OUTPUT_DIR'
export WORK_DIR='$env:WORK_DIR'
export VCPKG_ROOT='$env:VCPKG_ROOT'
export VCPKG_INSTALLED='$env:VCPKG_INSTALLED'
export LIB='$env:LIB'
export INCLUDE='$env:INCLUDE'
export LIBPATH='$env:LIBPATH'
echo "[tempfile] TARGET_PLATFORM=[`$TARGET_PLATFORM] VERSION=[`$VERSION] cwd=[`$(pwd)]" >&2
declare -p TARGET_PLATFORM VERSION OUTPUT_DIR WORK_DIR 2>&1 | sed 's/^/[tempfile] /' >&2
cd /c/Users/misi/yetty-builds/src
# Use `env` explicitly so the child definitely sees them — sidesteps
# any bash weirdness about `export` propagating across `exec`.
# TMP/TEMP are needed by clang-cl for intermediate files.
exec env \
    TARGET_PLATFORM="`$TARGET_PLATFORM" \
    VERSION="`$VERSION" \
    OUTPUT_DIR="`$OUTPUT_DIR" \
    WORK_DIR="`$WORK_DIR" \
    VCPKG_ROOT="`$VCPKG_ROOT" \
    VCPKG_INSTALLED="`$VCPKG_INSTALLED" \
    LIB="`$LIB" \
    INCLUDE="`$INCLUDE" \
    LIBPATH="`$LIBPATH" \
    PATH="`$PATH" \
    TMP="`${TMP:-/tmp}" \
    TEMP="`${TEMP:-/tmp}" \
    USERPROFILE="`$USERPROFILE" \
    APPDATA="`$APPDATA" \
    LOCALAPPDATA="`$LOCALAPPDATA" \
    SYSTEMROOT="`$SYSTEMROOT" \
    bash ./build-tools/assets/qemu/build.sh
"@
    # Windows line endings (CRLF) break bash parsing — especially after
    # quoted values, where the trailing \r ends up inside the var. Force
    # LF-only, UTF-8 no-BOM.
    $Utf8NoBom = New-Object System.Text.UTF8Encoding $false
    $ScriptBodyLf = $ScriptBody.Replace("`r`n", "`n").Replace("`r", "`n")
    [System.IO.File]::WriteAllText($TempScript, $ScriptBodyLf, $Utf8NoBom)
    $TempScriptPosix = To-PosixPath $TempScript
    Write-Host "==> handoff: TARGET_PLATFORM=$env:TARGET_PLATFORM VERSION=$env:VERSION"
    Write-Host "==> bash script at $TempScript ($TempScriptPosix)"
    Write-Host "==> first 15 lines:"
    Get-Content $TempScript -TotalCount 15 | ForEach-Object { Write-Host "    $_" }
    try {
        & $GitBash --noprofile --norc $TempScriptPosix
    } finally {
        # Keep the tempfile for post-mortem if the build failed
        if ($LASTEXITCODE -eq 0) {
            Remove-Item -Force -ErrorAction SilentlyContinue $TempScript
        }
    }
    if ($LASTEXITCODE -ne 0) { throw "build.sh failed with exit $LASTEXITCODE" }
} finally {
    Pop-Location
}
