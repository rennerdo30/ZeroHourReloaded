<#
.SYNOPSIS
    Configures and builds ZeroHourReloaded in native 64-bit (x64) with Ninja, sccache (C++ & Rust), and Vulkan 1.3.
.EXAMPLE
    pwsh -File scripts/build-win64-vulkan.ps1 -Target core_wwlib,core_wwmath,zhr_ffi_cargo
    pwsh -File scripts/build-win64-vulkan.ps1 -Target z_generals -Config RelWithDebInfo
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo")]
    [string]$Config = "RelWithDebInfo",
    [string[]]$Target = @("zhr_ffi_cargo", "core_wwlib", "core_wwmath"),
    [switch]$Reconfigure,
    [switch]$KeepGoing
)

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$buildDir = Join-Path $repoRoot "build\win64-vulkan"

# Locate Visual Studio x64 environment
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe not found; install Visual Studio with C++ Desktop workload."
}
$vsPath = (& $vswhere -latest -property installationPath).Trim()
$vcvars64 = Join-Path $vsPath "VC\Auxiliary\Build\vcvars64.bat"
if (-not (Test-Path $vcvars64)) {
    throw "vcvars64.bat not found at $vcvars64"
}

# Import VS x64 environment variables into current PowerShell session
$envDump = cmd /c "`"$vcvars64`" >nul && set"
foreach ($line in $envDump) {
    if ($line -match '^([^=]+)=(.*)$') {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2], "Process")
    }
}

# Prepend local Ninja if not already in PATH
if (-not (Get-Command ninja -ErrorAction SilentlyContinue)) {
    $localNinja = "E:\Development\_tools\ninja"
    if (Test-Path (Join-Path $localNinja "ninja.exe")) {
        $env:PATH = "$localNinja;$env:PATH"
    }
}

# Locate sccache for C/C++ compiler caching (Rust picks it up from .cargo/config.toml)
$sccachePath = $null
$inPath = Get-Command sccache -ErrorAction SilentlyContinue
if ($inPath) {
    $sccachePath = $inPath.Source -replace '\\', '/'
} elseif (Test-Path "E:/Development/_tools/sccache") {
    $found = Get-ChildItem "E:/Development/_tools/sccache" -Recurse -Filter "sccache.exe" -ErrorAction SilentlyContinue |
        Sort-Object FullName -Descending | Select-Object -First 1
    if ($found) { $sccachePath = $found.FullName -replace '\\', '/' }
}

if ($sccachePath) {
    $env:SCCACHE_DIR = "E:/sccache"
    $env:SCCACHE_CACHE_SIZE = "50G"
}

if ($Reconfigure -or -not (Test-Path (Join-Path $buildDir "build.ninja"))) {
    $cmakeArgs = @(
        "-S", $repoRoot,
        "-B", $buildDir,
        "-G", "Ninja Multi-Config",
        "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON",
        "-DCMAKE_MSVC_DEBUG_INFORMATION_FORMAT=Embedded",
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded`$<$<CONFIG:Debug>:Debug>DLL",
        "-DZHR_USE_VULKAN=ON",
        "-DZHR_USE_SDL3=ON",
        "-DZHR_BUILD_RUST=ON",
        "-DRTS_BUILD_OPTION_FFMPEG=ON",
        "-DRTS_BUILD_OPTION_RETAIL_COMPATIBLE_GAME=OFF",
        "-DRTS_BUILD_GENERALS=OFF",
        "-DRTS_BUILD_ZEROHOUR=ON",
        "-DRTS_BUILD_CORE_TOOLS=OFF",
        "-DRTS_BUILD_ZEROHOUR_TOOLS=OFF",
        "-DRTS_FLAGS=/W3"
    )
    if ($sccachePath) {
        $cmakeArgs += "-DCMAKE_C_COMPILER_LAUNCHER=$sccachePath"
        $cmakeArgs += "-DCMAKE_CXX_COMPILER_LAUNCHER=$sccachePath"
    }
    Write-Host "=== Configuring CMake (win64-vulkan, Ninja + sccache) ===" -ForegroundColor Cyan
    & cmake @cmakeArgs
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$resolvedTargets = $Target | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '' }
Write-Host "=== Building targets [$($resolvedTargets -join ', ')] ($Config) ===" -ForegroundColor Cyan
$buildArgs = @("--build", $buildDir, "--config", $Config, "--target") + $resolvedTargets
if ($KeepGoing) {
    $buildArgs += @("--", "-k", "0")
}
& cmake @buildArgs
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
