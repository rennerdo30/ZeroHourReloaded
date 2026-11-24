<#
.SYNOPSIS
    Incrementally builds and launches the native 64-bit Vulkan 1.3 ZeroHourReloaded engine
    against your locally installed Command & Conquer: Generals - Zero Hour assets.

.DESCRIPTION
    Auto-discovers your Zero Hour game assets from:
      1. -AssetsDir argument or $env:ZHR_ASSETS_DIR
      2. Sibling/local asset folders (../GeneralsZeroHour-TSH, ./assets)
      3. Steam libraries (reads SteamPath from Registry + steamapps/libraryfolders.vdf on all drives)
      4. EA App / Origin / classic retail Registry keys
      5. Common Steam/EA Games folders across mounted drives

    Copies the built 64-bit executable as `generalszh-vulkan64.exe` (never overwriting any 32-bit
    `generalszh.exe`) and launches it natively on Vulkan 1.3.

.EXAMPLE
    cargo run
    pwsh -File scripts/run-win64-vulkan.ps1
    pwsh -File scripts/run-win64-vulkan.ps1 -NoShellMap -Validation
    pwsh -File scripts/run-win64-vulkan.ps1 -NoBuild
#>
[CmdletBinding()]
param(
    [ValidateSet("Debug", "RelWithDebInfo", "Release")]
    [string]$Config = "RelWithDebInfo",

    [string]$AssetsDir = "",

    [switch]$NoBuild,
    [switch]$Fullscreen,
    [switch]$NoShellMap,
    [switch]$Validation,
    [string[]]$ExtraArgs = @()
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot
$BuildDir = Join-Path $RepoRoot "build\win64-vulkan\GeneralsMD\$Config"
$BuiltExe = Join-Path $BuildDir "generalszh.exe"
$BuiltPdb = Join-Path $BuildDir "generalszh.pdb"

function Test-ZeroHourAssetsDir([string]$Path) {
    if (-not $Path -or -not (Test-Path $Path)) { return $false }
    return (Test-Path (Join-Path $Path "INIZH.big")) -or (Test-Path (Join-Path $Path "TexturesZH.big"))
}

function Find-ZeroHourAssetsDir([string]$ExplicitDir, [string]$RepoRootPath) {
    if ($ExplicitDir) {
        if (Test-ZeroHourAssetsDir $ExplicitDir) { return (Resolve-Path $ExplicitDir).Path }
        throw "Specified -AssetsDir '$ExplicitDir' does not contain Zero Hour assets (INIZH.big)."
    }

    if ($env:ZHR_ASSETS_DIR -and (Test-ZeroHourAssetsDir $env:ZHR_ASSETS_DIR)) {
        return (Resolve-Path $env:ZHR_ASSETS_DIR).Path
    }

    $candidates = [System.Collections.Generic.List[string]]::new()

    # 1. Local / sibling testbed folders
    $candidates.Add((Join-Path $RepoRootPath "assets"))
    $candidates.Add((Join-Path (Split-Path -Parent $RepoRootPath) "GeneralsZeroHour-TSH"))

    # 2. Steam Registry + libraryfolders.vdf (discovers Steam libraries on ANY drive)
    $steamRoots = [System.Collections.Generic.List[string]]::new()
    foreach ($regPath in @(
        "HKCU:\Software\Valve\Steam",
        "HKLM:\SOFTWARE\WOW6432Node\Valve\Steam",
        "HKLM:\SOFTWARE\Valve\Steam"
    )) {
        try {
            $props = Get-ItemProperty -Path $regPath -ErrorAction SilentlyContinue
            foreach ($propName in @("SteamPath", "InstallPath")) {
                $val = $props.$propName
                if ($val -and (Test-Path $val)) { $steamRoots.Add($val) }
            }
        } catch {}
    }

    $steamLibs = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    foreach ($sr in $steamRoots) {
        [void]$steamLibs.Add($sr)
        $vdf = Join-Path $sr "steamapps\libraryfolders.vdf"
        if (Test-Path $vdf) {
            foreach ($line in (Get-Content $vdf -ErrorAction SilentlyContinue)) {
                if ($line -match '"path"\s+"([^"]+)"') {
                    $libPath = $Matches[1] -replace '\\\\', '\'
                    if (Test-Path $libPath) { [void]$steamLibs.Add($libPath) }
                }
            }
        }
    }
    foreach ($lib in $steamLibs) {
        $candidates.Add((Join-Path $lib "steamapps\common\Command & Conquer Generals - Zero Hour"))
        $candidates.Add((Join-Path $lib "steamapps\common\Command and Conquer Generals Zero Hour"))
    }

    # 3. EA App / Origin / classic retail Registry keys
    foreach ($regPath in @(
        "HKLM:\SOFTWARE\WOW6432Node\EA Games\Command and Conquer Generals Zero Hour",
        "HKLM:\SOFTWARE\EA Games\Command and Conquer Generals Zero Hour",
        "HKLM:\SOFTWARE\WOW6432Node\Electronic Arts\EA Games\Command and Conquer Generals Zero Hour"
    )) {
        try {
            $props = Get-ItemProperty -Path $regPath -ErrorAction SilentlyContinue
            foreach ($propName in @("InstallPath", "Install Dir")) {
                $val = $props.$propName
                if ($val) { $candidates.Add($val) }
            }
        } catch {}
    }

    # 4. Common drive fallbacks
    foreach ($drive in (Get-PSDrive -PSProvider FileSystem -ErrorAction SilentlyContinue)) {
        $root = $drive.Root
        $candidates.Add((Join-Path $root "Program Files (x86)\Steam\steamapps\common\Command & Conquer Generals - Zero Hour"))
        $candidates.Add((Join-Path $root "Program Files\EA Games\Command and Conquer Generals Zero Hour\Command and Conquer Generals Zero Hour"))
        $candidates.Add((Join-Path $root "Games\Steam\steamapps\common\Command & Conquer Generals - Zero Hour"))
    }

    foreach ($c in $candidates) {
        if (Test-ZeroHourAssetsDir $c) {
            return (Resolve-Path $c).Path
        }
    }
    return $null
}

if (-not $NoBuild -or -not (Test-Path $BuiltExe)) {
    Write-Host "=== Incremental build: 64-bit Vulkan ZeroHourReloaded ($Config) ===" -ForegroundColor Cyan
    & (Join-Path $PSScriptRoot "build-win64-vulkan.ps1") -Config $Config -Target z_generals
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$ResolvedAssets = Find-ZeroHourAssetsDir -ExplicitDir $AssetsDir -RepoRootPath $RepoRoot
if (-not $ResolvedAssets) {
    Write-Host ""
    Write-Host "ERROR: Could not automatically locate your Zero Hour game assets (INIZH.big)." -ForegroundColor Red
    Write-Host "ZeroHourReloaded does not ship copyrighted game assets." -ForegroundColor Yellow
    Write-Host "Please install Command & Conquer: Generals - Zero Hour (Steam or EA App), or run:" -ForegroundColor Yellow
    Write-Host '  $env:ZHR_ASSETS_DIR = "C:\Path\To\Zero Hour"' -ForegroundColor White
    Write-Host "  cargo run" -ForegroundColor White
    Write-Host "or:" -ForegroundColor Yellow
    Write-Host '  pwsh -File scripts/run-win64-vulkan.ps1 -AssetsDir "C:\Path\To\Zero Hour"' -ForegroundColor White
    exit 1
}

$LaunchExe = Join-Path $ResolvedAssets "generalszh-vulkan64.exe"
Copy-Item $BuiltExe $LaunchExe -Force
if (Test-Path $BuiltPdb) {
    Copy-Item $BuiltPdb (Join-Path $ResolvedAssets "generalszh.pdb") -Force
}

$gameArgs = @("-nologo")
if (-not $Fullscreen) { $gameArgs += "-win" }
if ($NoShellMap) { $gameArgs += "-noshellmap" }
if ($ExtraArgs) { $gameArgs += $ExtraArgs }

if ($Validation) {
    $env:ZHR_VK_VALIDATION = "1"
    $valLayer = "E:\Development\_tools\vulkan-validation\nightly-2026-11-19\dist"
    if (Test-Path $valLayer) {
        $env:VK_ADD_LAYER_PATH = $valLayer
    }
    Write-Host "Khronos Vulkan Validation Layers: ENABLED" -ForegroundColor Yellow
} else {
    Remove-Item Env:\ZHR_VK_VALIDATION -ErrorAction SilentlyContinue
}

Write-Host "=== Launching ZeroHourReloaded (64-bit Vulkan 1.3) ===" -ForegroundColor Green
Write-Host "  Exe:    $LaunchExe"
Write-Host "  Assets: $ResolvedAssets"
Write-Host "  Args:   $($gameArgs -join ' ')"

Start-Process -FilePath $LaunchExe -ArgumentList $gameArgs -WorkingDirectory $ResolvedAssets
