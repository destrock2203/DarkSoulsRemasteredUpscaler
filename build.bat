@echo off
rem Sets up the MSVC x64 environment and builds a preset (default: x64-release).
rem Usage: build.bat [x64-release|x64-debug]
setlocal
set "PRESET=%~1"
if "%PRESET%"=="" set "PRESET=x64-release"

rem vcvars needs vswhere.exe on PATH, otherwise cl is silently missing.
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"

rem Find a VS install that actually has vcvarsall.bat (skips partial installs).
set "VCVARS="
for /f "usebackq delims=" %%i in (`vswhere -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -find VC\Auxiliary\Build\vcvarsall.bat`) do set "VCVARS=%%i"
if not defined VCVARS (
    echo error: no Visual Studio install with the C++ x64 toolset found.
    exit /b 1
)
for %%i in ("%VCVARS%\..\..\..\..") do set "VSDIR=%%~fi"
echo Using %VSDIR%

call "%VCVARS%" x64 >nul || exit /b 1
set "PATH=%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin;%VSDIR%\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja;%PATH%"

where cl >nul 2>&1 || (echo error: cl not found after vcvars & exit /b 1)
where cmake >nul 2>&1 || (echo error: bundled cmake not found & exit /b 1)

pushd "%~dp0"
cmake --preset %PRESET% || (popd & exit /b 1)
cmake --build --preset %PRESET%
set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
