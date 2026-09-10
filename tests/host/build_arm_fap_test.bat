@echo off
setlocal
set "VCVARS="
for %%V in (2026 2022 18 17) do call :probe "%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
for %%V in (2026 2022 18 17) do call :probe "%ProgramFiles(x86)%\Microsoft Visual Studio\%%V\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined VCVARS exit /b 1
call "%VCVARS%" >nul
if errorlevel 1 exit /b %errorlevel%
cd /d "%~dp0..\.."
cl /nologo /std:c17 /W4 /D_CRT_SECURE_NO_WARNINGS /LD tests\host\arm_fap_vm_dll.c /Fe:build_host\arm_fap_vm.dll /Fo:build_host\ /link /IMPLIB:build_host\arm_fap_vm.lib
exit /b %errorlevel%
:probe
if defined VCVARS exit /b 0
if exist %1 set "VCVARS=%~1"
exit /b 0
