@echo off
rem Double-click helper. Passes the mode through to deploy.ps1.
rem   deploy.bat doctor   |  deploy.bat setup  |  deploy.bat flash  |  deploy.bat all

setlocal
cd /d "%~dp0"

if "%~1"=="" (
    echo.
    echo   GloaMind eyes
    echo.
    echo   1. doctor  - check what is installed   ^(start here^)
    echo   2. setup   - install everything
    echo   3. flash   - compile and upload
    echo   4. all     - setup, then flash
    echo.
    set /p choice=  Choose 1-4:
    if "%choice%"=="1" set MODE=doctor
    if "%choice%"=="2" set MODE=setup
    if "%choice%"=="3" set MODE=flash
    if "%choice%"=="4" set MODE=all
    if not defined MODE set MODE=doctor
) else (
    set MODE=%~1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0deploy.ps1" -Mode %MODE%
echo.
pause
