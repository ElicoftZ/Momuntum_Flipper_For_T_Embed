@echo off
rem Builds and runs the Momentum core host tests with MSVC.
rem lib/momentum/settings_core.c is deliberately free of furi and ESP-IDF
rem dependencies so its logic can be tested without hardware.
rem
rem Note: parenthesised if-blocks are avoided below because the MSVC path
rem contains "(x86)", whose closing paren terminates such a block early.
setlocal EnableExtensions

set "REPO=%~dp0..\.."
set "VCROOT=%ProgramFiles(x86)%\Microsoft Visual Studio"
set "VCVARS="

for %%V in (2026 2022 18 17) do call :probe "%VCROOT%\%%V\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
for %%V in (2026 2022 18 17) do call :probe "%VCROOT%\%%V\Community\VC\Auxiliary\Build\vcvars64.bat"
for %%V in (2026 2022 18 17) do call :probe "%VCROOT%\%%V\Professional\VC\Auxiliary\Build\vcvars64.bat"

if defined VCVARS goto :have_vcvars
echo ERROR: MSVC build tools were not found under:
echo   %VCROOT%
echo Install the "Desktop development with C++" workload, then try again.
exit /b 1

:probe
if defined VCVARS exit /b 0
if exist %1 set "VCVARS=%~1"
exit /b 0

:have_vcvars
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%

if not exist "%REPO%\build_host" mkdir "%REPO%\build_host"

cl /nologo /std:c17 /W4 /WX /D_CRT_SECURE_NO_WARNINGS /I "%REPO%\lib" ^
    "%REPO%\tests\host\momentum_core_test.c" ^
    "%REPO%\lib\momentum\settings_core.c" ^
    /Fe:"%REPO%\build_host\momentum_core_test.exe" ^
    /Fo:"%REPO%\build_host\\"
if errorlevel 1 exit /b %errorlevel%

"%REPO%\build_host\momentum_core_test.exe"
exit /b %errorlevel%
