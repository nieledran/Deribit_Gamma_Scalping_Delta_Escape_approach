@echo off
setlocal
set "APP_DIR=%~dp0"
if exist "%~dp0gamma-dist\DeribitGamma.exe" set "APP_DIR=%~dp0gamma-dist\"
if not exist "%APP_DIR%DeribitGamma.exe" (
    echo Download the Windows x64 ZIP from this repository's Releases page.
    echo Extract the entire ZIP, then run start.cmd in the extracted folder.
    echo Building from source? See README.md for build instructions.
    pause
    exit /b 1
)
start "Deribit Gamma" /D "%APP_DIR%." "%APP_DIR%DeribitGamma.exe" %*
