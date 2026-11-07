@echo off
if "%~1"=="--run" goto :run
cmd /k "%~f0" --run
exit

:run
setlocal enabledelayedexpansion
cd /d "%~dp0"
title Roblox Launcher - C Build

echo.
echo  ================================================================
echo   ROBLOX LAUNCHER  ^|  PURE C BUILD
echo  ================================================================
echo.

where cl >nul 2>&1
if errorlevel 1 (
    echo  [..] cl.exe not in PATH -- locating Visual Studio...
    set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
    if not exist "!VSWHERE!" (
        echo  [ERR] vswhere.exe not found.
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

if not exist "deps\" mkdir deps

if not exist "deps\webview\" (
    echo  [1/2]  webview            downloading...
    curl -L -o deps\webview.zip "https://github.com/webview/webview/archive/refs/heads/master.zip" --silent --show-error
    if errorlevel 1 ( echo  [ERR] Download failed. & goto :fail )
    powershell -NoProfile -Command "Expand-Archive -Force deps\webview.zip deps\webview_tmp" >nul
    powershell -NoProfile -Command "Move-Item deps\webview_tmp\webview-master deps\webview" >nul
    powershell -NoProfile -Command "Remove-Item -Recurse -Force deps\webview_tmp" >nul
    del deps\webview.zip
    echo  [1/2]  webview            ready.
) else (
    echo  [1/2]  webview            cached.
)

if not exist "deps\webview2\" (
    echo  [2/2]  WebView2 SDK       downloading...
    curl -L -o deps\webview2.zip "https://www.nuget.org/api/v2/package/Microsoft.Web.WebView2" --silent --show-error
    if errorlevel 1 ( echo  [ERR] Download failed. & goto :fail )
    powershell -NoProfile -Command "Expand-Archive -Force deps\webview2.zip deps\webview2" >nul
    del deps\webview2.zip
    echo  [2/2]  WebView2 SDK       ready.
) else (
    echo  [2/2]  WebView2 SDK       cached.
)

echo.
echo  ---- COMPILING (C + webview.cc) ------------------------------------
echo.

cl /nologo /std:c17 /O2 /W3 /TC /D_CRT_SECURE_NO_WARNINGS /DWEBVIEW_STATIC launcher.c ^
    /I "deps\webview\core\include" ^
    /I "deps\webview2\build\native\include" ^
    /I "." ^
    /Fo:launcher_c.obj ^
    /c

if errorlevel 1 goto :fail

cl /nologo /std:c++17 /O2 /W3 /EHsc /DWEBVIEW_STATIC ^
    "deps\webview\core\src\webview.cc" ^
    /I "deps\webview\core\include" ^
    /I "deps\webview2\build\native\include" ^
    /Fo:webview.obj ^
    /c

if errorlevel 1 goto :fail

link /nologo /SUBSYSTEM:WINDOWS ^
    launcher_c.obj webview.obj ^
    winhttp.lib ole32.lib shlwapi.lib ^
    shell32.lib advapi32.lib user32.lib version.lib ^
    "deps\webview2\build\native\x64\WebView2LoaderStatic.lib" ^
    /OUT:launcher.exe

if errorlevel 1 goto :fail

if exist launcher_c.obj del /Q launcher_c.obj
if exist webview.obj    del /Q webview.obj
if exist launcher.exp   del /Q launcher.exp
if exist launcher.lib   del /Q launcher.lib

echo.
echo  ----------------------------------------------------------------
echo   [OK]  launcher.exe is ready.
echo  ----------------------------------------------------------------
echo.
pause
exit /b 0

:fail
echo.
echo  [FAILED]  See errors above.
pause
exit /b 1
