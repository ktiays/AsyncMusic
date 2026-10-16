@echo off
rem Builds the Windows app. Usage: build.cmd [Debug|Release] [x64|ARM64]
setlocal
set CONFIG=%~1
if "%CONFIG%"=="" set CONFIG=Debug
set PLATFORM=%~2
if "%PLATFORM%"=="" set PLATFORM=x64
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -requires Microsoft.Component.MSBuild -find MSBuild\**\Bin\amd64\MSBuild.exe`) do set MSBUILD=%%i
if not defined MSBUILD (
    echo MSBuild not found & exit /b 1
)
cd /d "%~dp0"
"%MSBUILD%" AsyncMusic.slnx -t:restore -p:RestorePackagesConfig=true -nologo -v:minimal || exit /b 1
"%MSBUILD%" AsyncMusic.slnx -p:Configuration=%CONFIG% -p:Platform=%PLATFORM% -nologo -v:minimal -m || exit /b 1
