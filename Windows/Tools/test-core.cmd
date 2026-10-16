@echo off
rem Builds Core with MSVC and runs its tests: test-core.cmd [Debug|Release]
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Release
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do set VS=%%i
call "%VS%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0..\..\Core"
cmake -S . -B build-windows -G Ninja -DCMAKE_BUILD_TYPE=%CONFIG% >nul || exit /b 1
cmake --build build-windows || exit /b 1
build-windows\asyncmusic_core_tests.exe
