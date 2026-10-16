@echo off
rem Builds the engine and the command line tool with CMake: build-cli.cmd [Debug|Release]
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0.."
cmake -S . -B build-cli -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% >nul || exit /b 1
cmake --build build-cli || exit /b 1
