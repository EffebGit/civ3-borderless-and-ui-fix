@echo off
rem One-off: same as build.bat but emits a linker map so an RVA can be resolved to a symbol.
rem Use when a crash log gives you a fault offset inside this DLL (e.g. "opengl32.dll+0x3C5F").
setlocal
set "ROOT=%~dp0.."
set "ROOT=%ROOT:\=/%"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS=%%i"
call "%VS%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul
cd /d "%ROOT%\build"
cl /nologo /O2 /MT /W3 /D_CRT_SECURE_NO_WARNINGS /LD "%ROOT%\src\civ3bl.c" /Fe:mapbuild.dll ^
   /link /DEF:"%ROOT%\src\opengl32.def" /SUBSYSTEM:WINDOWS /MAP:opengl32.map ^
   user32.lib gdi32.lib kernel32.lib shell32.lib opengl32_real.lib
