<#
.SYNOPSIS
    Optionally downloads permissive open-weight ONNX remastering models locally for ZeroHourReloaded.
.DESCRIPTION
    ZeroHourReloaded works out-of-the-box using its built-in pure-Rust edge-directed 2x super-resolution
    and Sobel PBR synthesizer (`rust/zhr-remaster`). For players who want to stage optional open-weight
    ONNX neural models locally (never committed to git), this script prepares `%LOCALAPPDATA%\ZeroHourReloaded\models`
    and fetches permissive open-license weights.
.NOTES
    Models & Licenses:
      - DeepBump (MIT): https://github.com/HugoTini/DeepBump (albedo -> normal/height ONNX CNN)
      - 4x-PBRify_RPLKSRd (CC0-1.0 Public Domain): https://github.com/Kim2091/PBRify_Remix
      - 2xNomosUni (CC-BY-4.0): https://github.com/Phhofm/models
#>
[CmdletBinding()]
param(
    [string]$ModelsDir = "$env:LOCALAPPDATA\ZeroHourReloaded\models",
    [switch]$DownloadDeepBump
)

$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path $ModelsDir | Out-Null

Write-Host "ZeroHourReloaded Local Open-Weight Model Directory:" -ForegroundColor Cyan
Write-Host "  $ModelsDir"
Write-Host ""
Write-Host "Built-in pure-Rust 2x edge-directed remastering + BLAKE3 cache is already active with zero downloads." -ForegroundColor Green
Write-Host "Optional open-weight ONNX models recognized in this folder:"
Write-Host "  1. deepbump256.onnx         (MIT License, HugoTini/DeepBump)"
Write-Host "  2. 4x-PBRify_RPLKSRd.onnx   (CC0-1.0 Public Domain, Kim2091/PBRify_Remix)"
Write-Host "  3. 2xNomosUni_span.onnx     (CC-BY-4.0, Phhofm/models)"

if ($DownloadDeepBump) {
    $url = "https://raw.githubusercontent.com/HugoTini/DeepBump/master/deepbump256.onnx"
    $dst = Join-Path $ModelsDir "deepbump256.onnx"
    Write-Host "`nDownloading DeepBump (MIT) -> $dst ..." -ForegroundColor Yellow
    Invoke-WebRequest -Uri $url -OutFile $dst
    Write-Host "Downloaded $([math]::Round((Get-Item $dst).Length / 1MB, 2)) MiB successfully." -ForegroundColor Green
} else {
    Write-Host "`nTip: pass -DownloadDeepBump to fetch DeepBump (MIT) automatically, or place .onnx models in $ModelsDir." -ForegroundColor DarkGray
}
