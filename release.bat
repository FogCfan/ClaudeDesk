@echo off
rem ---------------------------------------------------------------------------
rem  Compile ClaudeDesk en Release (optimise, sans console) puis lance l application.
rem    release.bat          compile puis lance
rem    release.bat build    compile seulement
rem  Double-cliquer dessus suffit. L'executable : build\release\ClaudeDesk.exe
rem ---------------------------------------------------------------------------
setlocal
cd /d "%~dp0"
set "PATH=C:\msys64\ucrt64\bin;%PATH%"

where cmake >nul 2>nul
if errorlevel 1 (
    echo [ERREUR] cmake introuvable. Installer MSYS2 UCRT64 puis :
    echo   pacman -S mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,sdl3,nlohmann-json,freetype}
    pause
    exit /b 1
)

if not exist "build\release\build.ninja" (
    echo == Configuration Release - la premiere fois, ImGui est telecharge ==
    cmake --preset release
    if errorlevel 1 goto :fail
)

echo == Compilation Release ==
cmake --build --preset release
if errorlevel 1 goto :fail

if /i "%~1"=="build" (
    echo OK : build\release\ClaudeDesk.exe
    exit /b 0
)
start "" "build\release\ClaudeDesk.exe"
exit /b 0

:fail
echo.
echo [ERREUR] La compilation a echoue (voir les messages ci-dessus).
pause
exit /b 1
