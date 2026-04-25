@echo off
setlocal

if /I "%PROCESSOR_ARCHITECTURE%"=="ARM64" (
    set "PLATFORM=ARM64"
    set "VSARCH=arm64"
    set "BUILDDIR=local_altair\build-msvc-arm64"
    set "EXEPATH=local_altair\build-msvc-arm64\Release\altair-local.exe"
) else (
    set "PLATFORM=x64"
    set "VSARCH=amd64"
    set "BUILDDIR=local_altair\build-msvc"
    set "EXEPATH=local_altair\build-msvc\Release\altair-local.exe"
)

set "VSDEVCMD="
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

if exist "%VSWHERE%" (
    for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
        if exist "%%~I\Common7\Tools\VsDevCmd.bat" (
            set "VSDEVCMD=%%~I\Common7\Tools\VsDevCmd.bat"
        )
    )
)

if not defined VSDEVCMD (
    if exist "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" (
        set "VSDEVCMD=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat"
    )
)

if not defined VSDEVCMD (
    echo Could not locate VsDevCmd.bat. Install Visual Studio 2022 Build Tools with Desktop development with C++.
    exit /b 1
)

call "%VSDEVCMD%" -arch=%VSARCH%
if errorlevel 1 exit /b %errorlevel%

cmake -S local_altair -B "%BUILDDIR%" -G "Visual Studio 17 2022" -A %PLATFORM%
if errorlevel 1 exit /b %errorlevel%

cmake --build "%BUILDDIR%" --config Release
if errorlevel 1 exit /b %errorlevel%

"%EXEPATH%"
exit /b %errorlevel%