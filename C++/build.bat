@echo off
if "%~1"=="--run" goto :run
cmd /k "%~f0" --run
exit

:run
setlocal enabledelayedexpansion
cd /d "%~dp0"
title Roblox Launcher - Build

echo.
echo  $$$$$$\                                     $$\   $$\    $$\  $$$$$$\
echo  $$  __$$\                                    $$ ^|  $$ ^|   $$ ^|$$  __$$\
echo  $$ /  \__^| $$$$$$\  $$$$$$\$$$$\   $$$$$$\ $$$$$$\ $$ ^|   $$ ^|\__/  $$ ^|
echo  $$ ^|      $$  __$$\ $$  _$$  _$$\ $$  __$$\\_$$  _^|\$$\  $$  ^| $$$$$$  ^|
echo  $$ ^|      $$ /  $$ ^|$$ / $$ / $$ ^|$$$$$$$$ ^| $$ ^|   \$$\$$  / $$  ____/
echo  $$ ^|  $$\ $$ ^|  $$ ^|$$ ^| $$ ^| $$ ^|$$   ____^| $$ ^|$$\ \$$$  /  $$ ^|
echo  \$$$$$$  ^|\$$$$$$  ^|$$ ^| $$ ^| $$ ^|\$$$$$$$\  \$$$$  ^| \$  /   $$$$$$$$\
echo   \______/  \______/ \__^| \__^| \__^| \_______^|  \____/   \_/    \________^|
echo.
echo  ----------------------------------------------------------------
echo   C EDITION  ^|  BUILD SYSTEM
echo  ----------------------------------------------------------------
echo.

where cl >nul 2>&1
if errorlevel 1 (
    echo  [..] cl.exe not in PATH -- locating Visual Studio...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo  [ERR] vswhere.exe not found. Install Visual Studio 2019 or later.
        goto :fail
    )
    for /f "usebackq delims=" %%i in (`"!VSWHERE!" -latest -property installationPath`) do set "VS_INSTALL=%%i"
    if not defined VS_INSTALL (
        echo  [ERR] No Visual Studio installation found.
        goto :fail
    )
    set "VCVARS=!VS_INSTALL!\VC\Auxiliary\Build\vcvars64.bat"
    if not exist "!VCVARS!" (
        echo  [ERR] vcvars64.bat not found.
        goto :fail
    )
    echo  [..] Initializing x64 toolchain...
    call "!VCVARS!" >nul 2>&1
    echo  [OK] x64 environment ready.
) else (
    echo  [OK] x64 environment already active.
)
echo.

if not exist "Dependecies\" mkdir Dependecies

echo  ---- DEPENDENCIES --------------------------------------------------
echo.

if not exist "Dependecies\webview\" (
    echo  [1/3]  webview            downloading...
    curl -L -o Dependecies\webview.zip "https://github.com/webview/webview/archive/refs/heads/master.zip" --silent --show-error
    if errorlevel 1 ( echo  [ERR] Download failed. & goto :fail )
    echo  [1/3]  webview            extracting...
    powershell -NoProfile -Command "Expand-Archive -Force Dependecies\webview.zip Dependecies\webview_tmp" >nul
    powershell -NoProfile -Command "Move-Item Dependecies\webview_tmp\webview-master Dependecies\webview" >nul
    powershell -NoProfile -Command "Remove-Item -Recurse -Force Dependecies\webview_tmp" >nul
    del Dependecies\webview.zip
    echo  [1/3]  webview            ready.
) else (
    echo  [1/3]  webview            cached.
)

if not exist "Dependecies\webview2\" (
    echo  [2/3]  WebView2 SDK       downloading...
    curl -L -o Dependecies\webview2.zip "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2" --silent --show-error
    if errorlevel 1 ( echo  [ERR] Download failed. & goto :fail )
    echo  [2/3]  WebView2 SDK       extracting...
    powershell -NoProfile -Command "Expand-Archive -Force Dependecies\webview2.zip Dependecies\webview2" >nul
    del Dependecies\webview2.zip
    echo  [2/3]  WebView2 SDK       ready.
) else (
    echo  [2/3]  WebView2 SDK       cached.
)

if not exist "Dependecies\miniz\" (
    echo  [3/3]  miniz              downloading...
    curl -L -o Dependecies\miniz.zip "https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip" --silent --show-error
    if errorlevel 1 ( echo  [ERR] Download failed. & goto :fail )
    echo  [3/3]  miniz              extracting...
    powershell -NoProfile -Command "Expand-Archive -Force Dependecies\miniz.zip Dependecies\miniz_tmp" >nul
    powershell -NoProfile -Command "New-Item -ItemType Directory -Force Dependecies\miniz" >nul
    powershell -NoProfile -Command "Get-ChildItem -Recurse Dependecies\miniz_tmp -Filter miniz.h | Select-Object -First 1 | ForEach-Object { Copy-Item $_.FullName Dependecies\miniz\miniz.h }" >nul
    powershell -NoProfile -Command "Get-ChildItem -Recurse Dependecies\miniz_tmp -Filter miniz.c | Select-Object -First 1 | ForEach-Object { Copy-Item $_.FullName Dependecies\miniz\miniz.c }" >nul
    powershell -NoProfile -Command "Remove-Item -Recurse -Force Dependecies\miniz_tmp" >nul
    del Dependecies\miniz.zip
    echo  [3/3]  miniz              ready.
) else (
    echo  [3/3]  miniz              cached.
)

echo.
echo  ---- COMPILING -----------------------------------------------------
echo.

cl /EHsc /std:c++17 /O2 /W3 /nologo ^
    launcher.cpp ^
    "Dependecies\webview\core\src\webview.cc" ^
    "Dependecies\miniz\miniz.c" ^
    /I "Dependecies\webview\core\include" ^
    /I "Dependecies\webview2\build\native\include" ^
    /I "Dependecies\miniz" ^
    /I "." ^
    winhttp.lib ole32.lib shlwapi.lib ^
    shell32.lib advapi32.lib user32.lib version.lib ^
    "Dependecies\webview2\build\native\x64\WebView2LoaderStatic.lib" ^
    /Fe:launcher.exe ^
    /link /SUBSYSTEM:WINDOWS

if errorlevel 1 goto :fail

if exist launcher.obj  del /Q launcher.obj
if exist webview.obj   del /Q webview.obj
if exist miniz.obj     del /Q miniz.obj
if exist launcher.exp  del /Q launcher.exp
if exist launcher.lib  del /Q launcher.lib

echo.
echo  ----------------------------------------------------------------
echo   [OK]  launcher.exe is ready.
echo  ----------------------------------------------------------------
echo.
pause
exit /b 0

:fail
echo.
echo  ----------------------------------------------------------------
echo   [FAILED]  See errors above.
echo  ----------------------------------------------------------------
echo.
pause
exit /b 1
