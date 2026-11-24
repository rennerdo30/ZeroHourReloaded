@echo off
setlocal
title ZeroHourReloaded - Build and Play (64-bit Vulkan 1.3)
cd /d "%~dp0"
where cargo >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    cargo run -- %*
) else (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-win64-vulkan.ps1" %*
)
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ZeroHourReloaded failed to build or launch ^(exit code %ERRORLEVEL%^).
    pause
)
endlocal
