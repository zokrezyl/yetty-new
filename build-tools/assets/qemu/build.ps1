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

if (-not $env:VCPKG_ROOT)    { $env:VCPKG_ROOT    = "C:\Users\misi\vcpkg" }
if (-not $env:VCPKG_TRIPLET) { $env:VCPKG_TRIPLET = "x64-windows" }

$VcpkgInstalled = Join-Path $env:VCPKG_ROOT "installed\$($env:VCPKG_TRIPLET)"
if (-not (Test-Path $VcpkgInstalled)) {
    throw "vcpkg tree not found at $VcpkgInstalled"
}
$env:VCPKG_INSTALLED = $VcpkgInstalled.Replace('\', '/')

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

#-----------------------------------------------------------------------------
# Prepend extra tool dirs to PATH so Git Bash inherits them
#-----------------------------------------------------------------------------
$ExtraPath = @(
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
#-----------------------------------------------------------------------------
$GitBash = @(
    "C:\Program Files\Git\bin\bash.exe",
    "C:\Program Files\Git\usr\bin\bash.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $GitBash) { throw "Git Bash not found" }

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..\..")
Push-Location $RepoRoot
try {
    & $GitBash -lc "cd '$($RepoRoot.Path.Replace('\','/'))' && exec ./build-tools/assets/qemu/build.sh"
    if ($LASTEXITCODE -ne 0) { throw "build.sh failed with exit $LASTEXITCODE" }
} finally {
    Pop-Location
}
