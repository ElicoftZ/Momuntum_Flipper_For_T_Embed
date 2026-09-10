@echo off
setlocal
set "VCVARS="
for %%V in (2026 2022 18 17) do call :probe "%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
for %%V in (2026 2022 18 17) do call :probe "%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
cd /d "%~dp0..\.."
cl /nologo /std:c17 /Zc:preprocessor /D_CRT_SECURE_NO_WARNINGS /DESP_PLATFORM ^
    /FIhost_compiler.h ^
    /I tests\host\stubs /I components\furi\core /I components\mlib ^
    tests\host\furi_string_psram_test.c components\furi\core\furi_string.c ^
    /Fe:build_host\furi_string_psram_test.exe /Fo:build_host\
if errorlevel 1 exit /b %errorlevel%
build_host\furi_string_psram_test.exe
exit /b %errorlevel%

:probe
if defined VCVARS exit /b 0
if exist %1 set "VCVARS=%~1"
exit /b 0
