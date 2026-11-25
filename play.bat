@echo off
setlocal
title ZeroHourReloaded - Build and Play (64-bit Vulkan 1.3)
cd /d "%~dp0"
where pwsh >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-win64-vulkan.ps1" %*
    goto :done
)
where powershell >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\run-win64-vulkan.ps1" %*
    goto :done
)
where cargo >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    cargo run -- %*
    goto :done
)
echo Could not find pwsh, powershell, or cargo in PATH.
exit /b 1

:done
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo ZeroHourReloaded failed to build or launch ^(exit code %ERRORLEVEL%^).
    pause
)
endlocal
