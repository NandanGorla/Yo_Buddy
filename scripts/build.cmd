@echo off
rem Configure, build and test Buddy from a plain terminal.
rem Usage: scripts\build.cmd [debug|release] [notest]
rem CLion does not need this script: open the folder and pick the "msvc-debug" preset.
setlocal

set "CONFIG=%~1"
if "%CONFIG%"=="" set "CONFIG=debug"
set "PRESET=msvc-%CONFIG%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere.exe not found. Install Visual Studio Build Tools with the C++ workload.
  exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo No Visual Studio installation with the C++ tools was found.
  exit /b 1
)

call "%VSINSTALL%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if not defined VCPKG_ROOT (
  echo VCPKG_ROOT is not set. Install the "vcpkg package manager" Visual Studio component, or set VCPKG_ROOT to a vcpkg clone.
  exit /b 1
)

cd /d "%~dp0.."
cmake --preset %PRESET% || exit /b 1
cmake --build --preset %PRESET% || exit /b 1
if /i "%~2"=="notest" exit /b 0
ctest --preset %PRESET% || exit /b 1
