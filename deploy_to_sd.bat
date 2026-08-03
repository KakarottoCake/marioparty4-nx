@echo off
setlocal EnableExtensions DisableDelayedExpansion
title Mario Party 4 Switch - SD Card Installer

echo.
echo Mario Party 4 Switch - one-click SD card installer
echo.
echo This copies the port and creates:
echo   SD:\switch\marioparty4_switch.nro
echo   SD:\switch\files\...
echo.
echo The game assets must come from your own extracted copy of Mario Party 4.
echo This script does not download or provide copyrighted game data.
echo.

set "SCRIPT_DIR=%~dp0"
set "NRO_SOURCE="
set "ASSET_SOURCE="

rem Prefer a release/template folder where the batch file sits.
if exist "%SCRIPT_DIR%marioparty4_switch.nro" set "NRO_SOURCE=%SCRIPT_DIR%marioparty4_switch.nro"
if exist "%SCRIPT_DIR%files\data" set "ASSET_SOURCE=%SCRIPT_DIR%files"

rem Also support running the batch file from the repository root after building.
if not defined NRO_SOURCE if exist "%SCRIPT_DIR%src\platform\switch\marioparty4_switch.nro" set "NRO_SOURCE=%SCRIPT_DIR%src\platform\switch\marioparty4_switch.nro"

rem Convenience path for the project's local compiled template.
if not defined NRO_SOURCE if exist "D:\AI Projects\Compiled\MP4\marioparty4_switch.nro" set "NRO_SOURCE=D:\AI Projects\Compiled\MP4\marioparty4_switch.nro"
if not defined ASSET_SOURCE if exist "D:\AI Projects\Compiled\MP4\files\data" set "ASSET_SOURCE=D:\AI Projects\Compiled\MP4\files"

if not defined NRO_SOURCE (
    echo The compiled NRO was not found automatically.
    set /p "NRO_SOURCE=Drag marioparty4_switch.nro here, or type its full path: "
    set "NRO_SOURCE=%NRO_SOURCE:"=%"
)
if not exist "%NRO_SOURCE%" (
    echo.
    echo ERROR: NRO file not found.
    pause
    exit /b 1
)

if not defined ASSET_SOURCE (
    echo.
    echo The extracted game files folder was not found automatically.
    echo It must contain folders such as data, dll, and mess.
    set /p "ASSET_SOURCE=Drag the extracted files folder here, or press Enter to skip: "
    set "ASSET_SOURCE=%ASSET_SOURCE:"=%"
)
if defined ASSET_SOURCE if exist "%ASSET_SOURCE%\files\data" if not exist "%ASSET_SOURCE%\data" set "ASSET_SOURCE=%ASSET_SOURCE%\files"
if defined ASSET_SOURCE if not exist "%ASSET_SOURCE%" (
    echo.
    echo WARNING: The asset folder was not found. Continuing without assets.
    set "ASSET_SOURCE="
)

echo.
set "SD_ROOT="
set /p "SD_ROOT=Enter the SD card drive letter, for example E: "
set "SD_ROOT=%SD_ROOT:"=%"
set "SD_ROOT=%SD_ROOT:~0,2%"
if "%SD_ROOT:~1,1%"=="" set "SD_ROOT=%SD_ROOT%:"
if not exist "%SD_ROOT%\" (
    echo.
    echo ERROR: The drive %SD_ROOT% was not found.
    pause
    exit /b 1
)

echo.
echo Target: %SD_ROOT%\switch
choice /M "Is this the correct SD card"
if errorlevel 2 (
    echo Cancelled.
    exit /b 0
)

if not exist "%SD_ROOT%\switch" mkdir "%SD_ROOT%\switch"
if errorlevel 1 goto :copy_error
copy /Y "%NRO_SOURCE%" "%SD_ROOT%\switch\marioparty4_switch.nro" >nul
if errorlevel 1 goto :copy_error

if defined ASSET_SOURCE (
    if not exist "%SD_ROOT%\switch\files" mkdir "%SD_ROOT%\switch\files"
    robocopy "%ASSET_SOURCE%" "%SD_ROOT%\switch\files" /E /COPY:DAT /DCOPY:DAT /R:1 /W:1 /NFL /NDL /NJH /NJS /NP >nul
    if errorlevel 8 goto :copy_error
) else (
    if not exist "%SD_ROOT%\switch\files" mkdir "%SD_ROOT%\switch\files"
    echo.
    echo WARNING: No game assets were copied.
    echo Put your extracted files folder in %SD_ROOT%\switch\files before launching.
)

echo.
echo Done. Launch marioparty4_switch.nro through Homebrew Menu.
echo The game log should report: Total files found: 357
pause
exit /b 0

:copy_error
echo.
echo ERROR: Copy failed. Check that the SD card is writable and has enough space.
pause
exit /b 1
