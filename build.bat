@echo off
rem Builds and tests MQLConnector.dll (x64 Release) and copies it to ..\MQL\Libraries
rem when that folder exists (inside the FinCore repository).
rem   build.bat            configure, build, test, copy
rem   build.bat notest     skip ctest
setlocal

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found - install Visual Studio 2022 Build Tools.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSPATH=%%i"
)
if not defined VSPATH (
    echo MSVC x64 tools not found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
rem Prefer the CMake/Ninja bundled with Visual Studio: it verifies TLS against the Windows store,
rem whereas a MinGW/MSYS2 cmake earlier on PATH cannot download GoogleTest.
set "VSCMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake"
if exist "%VSCMAKE%\CMake\bin\cmake.exe" set "PATH=%VSCMAKE%\CMake\bin;%VSCMAKE%\Ninja;%PATH%"

pushd "%~dp0"
cmake --preset windows-x64-release || goto :fail
cmake --build --preset windows-x64-release || goto :fail
if /i not "%~1"=="notest" (
    ctest --preset windows-x64-release || goto :fail
)
if exist "..\MQL\Libraries" (
    copy /y "build\windows-x64-release\MQLConnector.dll" "..\MQL\Libraries\" >nul || goto :fail
    echo Copied MQLConnector.dll to ..\MQL\Libraries
)
popd
exit /b 0

:fail
set "ERR=%ERRORLEVEL%"
popd
exit /b %ERR%
