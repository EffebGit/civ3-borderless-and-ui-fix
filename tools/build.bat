@echo off
rem Build Civ3 Borderless (32-bit proxy DLL).
rem Requirements: Visual Studio 2019/2022 with the C++ x86 tools + a Windows SDK.
setlocal
set "ROOT=%~dp0.."
set "ROOT=%ROOT:\=/%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [X] vswhere.exe not found - install Visual Studio with the Desktop C++ workload.
    exit /b 1
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo [X] No Visual Studio installation with the x86 C++ tools found.
    exit /b 1
)

call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
if errorlevel 1 (echo [X] vcvarsall failed & exit /b 1)

if not exist "%ROOT%\build" mkdir "%ROOT%\build"
pushd "%ROOT%\build"

rem An import library for the real OpenGL DLL: MSVC will not create forwarders
rem (foo=opengl32_real.foo) without one, it reports LNK2001 on every symbol.
lib /nologo /def:"%ROOT%\src\opengl32_real.def" /out:opengl32_real.lib /machine:x86
if errorlevel 1 (popd & echo [X] lib failed & exit /b 1)

cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /LD "%ROOT%\src\civ3bl.c" /Fe:opengl32.dll ^
   /link /DEF:"%ROOT%\src\opengl32.def" /SUBSYSTEM:WINDOWS ^
   user32.lib gdi32.lib kernel32.lib shell32.lib opengl32_real.lib
if errorlevel 1 (popd & echo [X] compile/link failed & exit /b 1)

popd
echo.
echo Built: build\opengl32.dll
echo Deploy with dist\install.bat
