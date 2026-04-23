# Yetty Windows Build Script (PowerShell)
# Usage: build.ps1 [-Config Debug|Release] [-Clean] [-ConfigureOnly]
#        build.ps1 debug
#        build.ps1 release clean
#        build.ps1 release configure

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Args
)

$ErrorActionPreference = 'Stop'

$Config = 'Release'
$Clean = $false
$ConfigureOnly = $false

foreach ($a in $Args) {
    switch -Regex ($a.ToLower()) {
        '^debug$'     { $Config = 'Debug' }
        '^release$'   { $Config = 'Release' }
        '^clean$'     { $Clean = $true }
        '^configure$' { $ConfigureOnly = $true }
        default       { Write-Warning "Unknown argument: $a" }
    }
}

# --- Tool path discovery ---
$extraPaths = @()

# GNU Make
$makeCandidates = @(
    'C:\Program Files (x86)\GnuWin32\bin',
    'C:\Program Files\Git\mingw64\bin',
    'C:\ProgramData\chocolatey\bin',
    "$env:ProgramFiles\GnuWin32\bin"
)
foreach ($p in $makeCandidates) {
    if (Test-Path $p) { $extraPaths += $p }
}

# NASM
$nasmCandidates = @(
    'C:\Program Files\NASM',
    'C:\Program Files (x86)\NASM',
    "$env:LOCALAPPDATA\bin\NASM"
)
foreach ($p in $nasmCandidates) {
    if (Test-Path $p) { $extraPaths += $p }
}

# Ninja / CMake (usually already on PATH when installed via winget/VS)
$ninjaCandidates = @(
    "$env:ProgramFiles\CMake\bin",
    'C:\Program Files\CMake\bin'
)
foreach ($p in $ninjaCandidates) {
    if (Test-Path $p) { $extraPaths += $p }
}

if ($extraPaths.Count -gt 0) {
    $env:PATH = ($extraPaths -join ';') + ';' + $env:PATH
}

# --- Paths ---
$ProjectRoot = (Get-Location).Path
$ConfigLower = $Config.ToLower()
$BuildDir = Join-Path $ProjectRoot "build-windows-ytrace-$ConfigLower"

Write-Host "Yetty Windows Build"
Write-Host "  Project Root: $ProjectRoot"
Write-Host "  Build Dir:    $BuildDir"
Write-Host "  Config:       $Config"
Write-Host ""

# --- MSVC environment setup ---
function Test-Command($name) {
    $null -ne (Get-Command $name -ErrorAction SilentlyContinue)
}

function Import-VcVars {
    param([string]$VcVarsPath, [string]$Arch = 'x64')
    Write-Host "Loading MSVC env from: $VcVarsPath"
    $tmp = [System.IO.Path]::GetTempFileName() + '.txt'
    try {
        & cmd.exe /c "`"$VcVarsPath`" $Arch > nul && set" | Out-File -Encoding ascii $tmp
        Get-Content $tmp | ForEach-Object {
            if ($_ -match '^([^=]+)=(.*)$') {
                Set-Item -Path "Env:$($Matches[1])" -Value $Matches[2]
            }
        }
    } finally {
        Remove-Item $tmp -ErrorAction SilentlyContinue
    }
}

if (-not (Test-Command 'cl.exe')) {
    Write-Host "Setting up MSVC environment..."
    $vcvarsCandidates = @(
        'C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvarsall.bat',
        'C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvarsall.bat'
    )
    $vcvars = $vcvarsCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
    if (-not $vcvars) {
        Write-Error "Could not find Visual Studio vcvarsall.bat. Install VS Build Tools: winget install Microsoft.VisualStudio.2022.BuildTools"
        exit 1
    }
    Import-VcVars -VcVarsPath $vcvars -Arch 'x64'
}

if (-not (Test-Command 'cl.exe')) {
    Write-Error "cl.exe not found after MSVC setup"
    exit 1
}

# --- Clean ---
if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "Cleaning build directory..."
    Remove-Item -Recurse -Force $BuildDir
}

# --- Configure ---
$needsConfigure = -not (Test-Path (Join-Path $BuildDir 'build.ninja'))
if ($needsConfigure) {
    $slnFiles = @(Get-ChildItem -Path $BuildDir -Filter '*.sln' -ErrorAction SilentlyContinue)
    if ($slnFiles.Count -gt 0) { $needsConfigure = $false }
}

if ($needsConfigure) {
    Write-Host "Configuring CMake..."
    if (Test-Command 'ninja') {
        & cmake -B $BuildDir -G 'Ninja' "-DCMAKE_BUILD_TYPE=$Config" '-DWEBGPU_BACKEND=dawn' $ProjectRoot
    } else {
        & cmake -B $BuildDir -G 'Visual Studio 17 2022' -A x64 '-DWEBGPU_BACKEND=dawn' $ProjectRoot
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "CMake configuration failed"
        exit 1
    }
}

if ($ConfigureOnly) {
    Write-Host "Configuration complete."
    exit 0
}

# --- Build ---
Write-Host "Building..."
& cmake --build $BuildDir --config $Config --parallel
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed"
    exit 1
}

Write-Host ""
Write-Host "Build complete!"

$exeCandidates = @(
    (Join-Path $BuildDir 'yetty.exe'),
    (Join-Path $BuildDir (Join-Path $Config 'yetty.exe'))
)
$exe = $exeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if ($exe) {
    Write-Host "Executable: $exe"
} else {
    Write-Warning "yetty.exe not found in expected locations"
}
