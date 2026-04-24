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
    "C:\Strawberry\perl\bin",                   # perl for QEMU scripts
    "$VcpkgInstalled\tools\pkgconf"             # pkgconf.exe (if vcpkg pkgconf installed)
)
# vcpkg downloads an msys2 with pkgconf under it; fall back to that.
$Msys2PkgConf = Get-ChildItem -Path "$env:VCPKG_ROOT\downloads\tools\msys2" `
    -Recurse -Filter "pkgconf.exe" -ErrorAction SilentlyContinue |
    Select-Object -First 1
if ($Msys2PkgConf) { $ExtraPath += (Split-Path $Msys2PkgConf.FullName) }

$env:PATH = ($ExtraPath -join ";") + ";" + $env:PATH

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
    & $GitBash --noprofile --norc -c "export PATH='$PosixPath'; cd '$RepoRootPosix' && exec ./build-tools/assets/qemu/build.sh"
    if ($LASTEXITCODE -ne 0) { throw "build.sh failed with exit $LASTEXITCODE" }
} finally {
    Pop-Location
}
