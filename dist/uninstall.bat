@echo off
setlocal
set "GAME=%~1"
if "%GAME%"=="" set "GAME=%~dp0"
set "GAME=%GAME:\=/%"
if "%GAME:~-1%"=="/" set "GAME=%GAME:~0,-1%"

echo [*] Target: %GAME%
if exist "%GAME%\opengl32.dll"      del /q "%GAME%\opengl32.dll"      && echo [-] removed opengl32.dll
if exist "%GAME%\opengl32_real.dll" del /q "%GAME%\opengl32_real.dll" && echo [-] removed opengl32_real.dll
if exist "%GAME%\civ3bl.log"        del /q "%GAME%\civ3bl.log"

if exist "%GAME%\conquests.ini.bak" (
    copy /y "%GAME%\conquests.ini.bak" "%GAME%\conquests.ini" >nul
    echo [+] restored conquests.ini from backup
)

echo.
echo The game is back to stock. civ3bl.ini was left in place; delete it if you like.
pause
