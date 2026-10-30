@echo off
setlocal enabledelayedexpansion
rem Civ3 Borderless - installer
rem Usage: install.bat ["<path to Conquests folder>"]

set "GAME=%~1"
if "%GAME%"=="" set "GAME=%~dp0"
set "GAME=%GAME:\=/%"
if "%GAME:~-1%"=="/" set "GAME=%GAME:~0,-1%"

if not exist "%GAME%\Civ3Conquests.exe" (
    echo [X] Civ3Conquests.exe not found in "%GAME%"
    echo     Pass the Conquests folder as the first argument, e.g.
    echo     install.bat "E:\SteamLibrary\steamapps\common\Sid Meier's Civilization III Complete\Conquests"
    pause
    exit /b 1
)

echo [*] Target: %GAME%

rem --- the real 32-bit OpenGL DLL this proxy forwards to -----------------------
set "SYSOPENGL=%WINDIR%\SysWOW64\opengl32.dll"
if not exist "%SYSOPENGL%" set "SYSOPENGL=%WINDIR%\System32\opengl32.dll"
copy /y "%SYSOPENGL%" "%GAME%\opengl32_real.dll" >nul || (
    echo [X] Could not copy the system opengl32.dll
    pause & exit /b 1
)
echo [+] opengl32_real.dll  (system OpenGL, forwarded to)

copy /y "%~dp0opengl32.dll" "%GAME%\opengl32.dll" >nul || (
    echo [X] Could not copy opengl32.dll - is the game still running?
    pause & exit /b 1
)
echo [+] opengl32.dll        (the patch)

if not exist "%GAME%\civ3bl.ini" (
    copy /y "%~dp0civ3bl.ini" "%GAME%\civ3bl.ini" >nul
    echo [+] civ3bl.ini         (new config)
) else (
    echo [=] civ3bl.ini         (kept your existing config^)
)

rem --- conquests.ini: KeepRes must be on, video mode off, no intro video ------
if not exist "%GAME%\conquests.ini.bak" (
    if exist "%GAME%\conquests.ini" copy /y "%GAME%\conquests.ini" "%GAME%\conquests.ini.bak" >nul
    echo [+] backed up conquests.ini
)

powershell -NoProfile -ExecutionPolicy Bypass -Command ^
 "$p = '%GAME%/conquests.ini';" ^
 "if (Test-Path $p) {" ^
 "  $l = Get-Content -LiteralPath $p;" ^
 "  $l = $l | Where-Object { $_ -notmatch '^\s*Video\s*Mode\s*=' };" ^
 "  if (-not ($l -match '^\s*KeepRes\s*=')) { $l += 'KeepRes=1' } else { $l = $l -replace '^\s*KeepRes\s*=.*','KeepRes=1' };" ^
 "  if (-not ($l -match '^\s*PlayIntro\s*=')) { $l += 'PlayIntro=0' } else { $l = $l -replace '^\s*PlayIntro\s*=.*','PlayIntro=0' };" ^
 "  Set-Content -LiteralPath $p -Value $l -Encoding ASCII }" 
echo [+] conquests.ini      (KeepRes=1, PlayIntro=0, Video Mode removed)

echo.
echo Done. Launch Civilization III Conquests normally.
echo If it looks wrong, set LogLevel=1 in civ3bl.ini and read civ3bl.log.
pause
