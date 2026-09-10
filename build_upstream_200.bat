@echo off
setlocal EnableExtensions
cd /d "%~dp0"
set "IDF_ROOT=%ESP_IDF_DIR%"
if not defined IDF_ROOT set "IDF_ROOT=C:\Espressif\frameworks\esp-idf-v5.4.1"
if not defined IDF_TOOLS_PATH for %%I in ("%IDF_ROOT%\..\..") do set "IDF_TOOLS_PATH=%%~fI"
set "PATH=%IDF_TOOLS_PATH%\python_env\idf5.4_py3.11_env\Scripts;%PATH%"
call "%IDF_ROOT%\export.bat" >nul || exit /b 1
rem Resolve the bundled tools explicitly: some Windows shells lose the PATH
rem additions made by ESP-IDF's generated activation batch file.
for /d %%T in ("%IDF_TOOLS_PATH%\tools\ninja\*") do if exist "%%~fT\ninja.exe" set "NINJA_EXE=%%~fT\ninja.exe"
if not defined NINJA_EXE (echo Ninja was not found under %IDF_TOOLS_PATH% & exit /b 1)
for /d %%T in ("%IDF_TOOLS_PATH%\tools\xtensa-esp-elf\*") do if exist "%%~fT\xtensa-esp-elf\bin\xtensa-esp32s3-elf-gcc.exe" set "XTENSA_BIN=%%~fT\xtensa-esp-elf\bin"
if not defined XTENSA_BIN (echo ESP32-S3 compiler was not found & exit /b 1)
set "PATH=%XTENSA_BIN%;%PATH%"
set "IDF_CCACHE_ENABLE=0"
set "FLIPPER_BOARD=lilygo_t_embed_cc1101"
set "MOMENTUM_RELEASE_BUILD=1"
set "BUILD_DIR=build_t_embed_200"
idf.py --no-ccache -B "%BUILD_DIR%" -DIDF_TARGET=esp32s3 -DCMAKE_MAKE_PROGRAM="%NINJA_EXE%" ^
    -DCMAKE_C_COMPILER="%XTENSA_BIN%\xtensa-esp32s3-elf-gcc.exe" ^
    -DCMAKE_CXX_COMPILER="%XTENSA_BIN%\xtensa-esp32s3-elf-g++.exe" ^
    -DCMAKE_ASM_COMPILER="%XTENSA_BIN%\xtensa-esp32s3-elf-gcc.exe" ^
    -DFLIPPER_BOARD=%FLIPPER_BOARD% ^
    -DSDKCONFIG="%BUILD_DIR%\sdkconfig" ^
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32s3;sdkconfig.variant_200" ^
    reconfigure || exit /b 1
"%NINJA_EXE%" -C "%BUILD_DIR%" -j 4 || exit /b 1
python -m esptool --chip esp32s3 merge_bin ^
    -o "%BUILD_DIR%\momentum_t_embed_2.0.0.bin" ^
    --flash_mode dio --flash_freq 80m --flash_size 16MB ^
    0x0 "%BUILD_DIR%\bootloader\bootloader.bin" ^
    0x8000 "%BUILD_DIR%\partition_table\partition-table.bin" ^
    0x10000 "%BUILD_DIR%\furi_esp32.bin" || exit /b 1
echo Built: %BUILD_DIR%\momentum_t_embed_2.0.0.bin
echo Build only. No device has been flashed.
endlocal
