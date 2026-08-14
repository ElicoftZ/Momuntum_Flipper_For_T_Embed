@echo off
setlocal EnableExtensions

cd /d "%~dp0"

set "IDF_ROOT=%ESP_IDF_DIR%"
if not defined IDF_ROOT set "IDF_ROOT=C:\Espressif\frameworks\esp-idf-v5.4.1"
if not defined IDF_TOOLS_PATH for %%I in ("%IDF_ROOT%\..\..") do set "IDF_TOOLS_PATH=%%~fI"

set "FLIPPER_BOARD=lilygo_t_embed_cc1101"
set "BUILD_DIR=build_t_embed"
set "MERGED_BIN=%BUILD_DIR%\momentum_t_embed_cc1101_merged.bin"

if not exist "%IDF_ROOT%\export.bat" (
    echo ERROR: ESP-IDF export script was not found at:
    echo   %IDF_ROOT%\export.bat
    echo Set ESP_IDF_DIR to your ESP-IDF v5.4.1 directory and try again.
    exit /b 1
)

rem A regular Command Prompt may not have the ESP-IDF Python environment on PATH.
set "IDF_PYTHON_DIR="
for /d %%P in ("%IDF_TOOLS_PATH%\python_env\idf5.4_py*") do if exist "%%~fP\Scripts\python.exe" set "IDF_PYTHON_DIR=%%~fP\Scripts"
if defined IDF_PYTHON_DIR set "PATH=%IDF_PYTHON_DIR%;%PATH%"

where python.exe >nul 2>&1
if errorlevel 1 (
    echo ERROR: No Python executable was found for ESP-IDF v5.4.1.
    echo Install the ESP-IDF Python environment, then try again.
    exit /b 1
)

echo [1/4] Activating ESP-IDF v5.4.1...
call "%IDF_ROOT%\export.bat"
if errorlevel 1 exit /b %errorlevel%

rem The port is supported and tested only on LilyGO T-Embed CC1101 / ESP32-S3.
rem Disable ccache because its Windows temporary-file handling is unreliable in some shells.
set "IDF_CCACHE_ENABLE="

echo [2/4] Configuring LilyGO T-Embed CC1101...
idf.py -B "%BUILD_DIR%" -DIDF_TARGET=esp32s3 -DFLIPPER_BOARD=%FLIPPER_BOARD% -DCCACHE_ENABLE=0 reconfigure
if errorlevel 1 exit /b %errorlevel%

echo [3/4] Building firmware...
idf.py -B "%BUILD_DIR%" -DIDF_TARGET=esp32s3 -DFLIPPER_BOARD=%FLIPPER_BOARD% -DCCACHE_ENABLE=0 build
if errorlevel 1 exit /b %errorlevel%

if not exist "%BUILD_DIR%\bootloader\bootloader.bin" (
    echo ERROR: bootloader.bin was not generated.
    exit /b 1
)
if not exist "%BUILD_DIR%\partition_table\partition-table.bin" (
    echo ERROR: partition-table.bin was not generated.
    exit /b 1
)
if not exist "%BUILD_DIR%\furi_esp32.bin" (
    echo ERROR: furi_esp32.bin was not generated.
    exit /b 1
)

echo [4/4] Creating one-file 16 MB flash image...
python -m esptool --chip esp32s3 merge_bin -o "%MERGED_BIN%" --flash_mode dio --flash_freq 80m --flash_size 16MB 0x0 "%BUILD_DIR%\bootloader\bootloader.bin" 0x8000 "%BUILD_DIR%\partition_table\partition-table.bin" 0x10000 "%BUILD_DIR%\furi_esp32.bin"
if errorlevel 1 exit /b %errorlevel%

echo.
echo Merged T-Embed CC1101 image created:
echo   %CD%\%MERGED_BIN%

if "%~1"=="" (
    echo.
    echo To build and flash in one step, pass a serial port:
    echo   %~nx0 COM14
    exit /b 0
)

echo.
echo Flashing merged image to %~1...
python -m esptool --chip esp32s3 -p "%~1" -b 460800 --before default_reset --after hard_reset write_flash --flash_size 16MB 0x0 "%MERGED_BIN%"
exit /b %errorlevel%
