@echo off
rem ---------------------------------------------------------------------------
rem  Compile ClaudeDesk en Debug, lance les tests unitaires, puis l application
rem  (avec une console qui affiche les logs).
rem    debug.bat            compile, teste, lance
rem    debug.bat build      compile + tests seulement
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

if not exist "build\debug\build.ninja" (
    cmake --preset debug
    if errorlevel 1 goto :fail
)
cmake --build --preset debug
if errorlevel 1 goto :fail
ctest --preset debug
if errorlevel 1 goto :fail

if /i "%~1"=="build" exit /b 0
"build\debug\ClaudeDesk.exe"
exit /b 0

:fail
echo.
echo [ERREUR] Echec de la compilation ou des tests (voir ci-dessus).
pause
exit /b 1
