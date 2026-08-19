@echo off
setlocal EnableExtensions
rem ---------------------------------------------------------------------------
rem Build the T-Embed CC1101 release image.
rem
rem   build_variant.bat [COMx]
rem
rem Produces momentum_t_embed_RELEASE.bin: bootloader, partition table and
rem firmware merged into one image. The SD card is set up by hand from
rem sdcard.zip. Pass a COM port to flash it too.
rem ---------------------------------------------------------------------------

cd /d "%~dp0"

set "PORT=%~1"

set "BUILD_DIR=build_t_embed_release"
set "TABLE=partitions_release_16mb.csv"
set "OUTNAME=momentum_t_embed_RELEASE.bin"

set "IDF_ROOT=%ESP_IDF_DIR%"
if not defined IDF_ROOT set "IDF_ROOT=C:\Espressif\frameworks\esp-idf-v5.4.1"
if not defined IDF_TOOLS_PATH for %%I in ("%IDF_ROOT%\..\..") do set "IDF_TOOLS_PATH=%%~fI"

set "IDF_PYTHON_DIR="
for /d %%P in ("%IDF_TOOLS_PATH%\python_env\idf5.4_py*") do if exist "%%~fP\Scripts\python.exe" set "IDF_PYTHON_DIR=%%~fP\Scripts"
if defined IDF_PYTHON_DIR set "PATH=%IDF_PYTHON_DIR%;%PATH%"

call "%IDF_ROOT%\export.bat" >nul || (echo ERROR: ESP-IDF export failed & exit /b 1)

set "FLIPPER_BOARD=lilygo_t_embed_cc1101"

echo.
echo === release : table %TABLE% -^> %BUILD_DIR% ===
echo.

rem The build keeps its own sdkconfig INSIDE the build dir. ESP-IDF otherwise
rem writes sdkconfig to the project root, where a stale copy silently overrides
rem sdkconfig.defaults and pins the wrong partition table.
rem
rem The table is a Kconfig option, so it must arrive through a defaults
rem fragment: `-DPARTITION_TABLE_CUSTOM_FILENAME=` sets an unrelated CMake cache
rem variable that nothing reads.
idf.py -B "%BUILD_DIR%" ^
    -DFLIPPER_BOARD=%FLIPPER_BOARD% ^
    -DSDKCONFIG="%BUILD_DIR%\sdkconfig" ^
    -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.variant_release" ^
    build || exit /b 1

rem Catch a stale sdkconfig pinning an older table. This layout is firmware
rem only, so an `assets` partition means the pre-manual-SD table is still in
rem force -- which would also mean an 8 MB factory instead of 4 MB.
python "%IDF_ROOT%\components\partition_table\gen_esp32part.py" ^
    "%BUILD_DIR%\partition_table\partition-table.bin" > "%BUILD_DIR%\partition_table_dump.txt"
findstr /b /c:"assets," "%BUILD_DIR%\partition_table_dump.txt" >nul
if not errorlevel 1 (
    echo.
    echo ERROR: stale partition table - it still has an 'assets' partition.
    echo        Delete %BUILD_DIR%\sdkconfig and rebuild.
    type "%BUILD_DIR%\partition_table_dump.txt"
    exit /b 1
)

python -m esptool --chip esp32s3 merge_bin -o "%BUILD_DIR%\%OUTNAME%" ^
    --flash_mode dio --flash_freq 80m --flash_size 16MB ^
    0x0 "%BUILD_DIR%\bootloader\bootloader.bin" ^
    0x8000 "%BUILD_DIR%\partition_table\partition-table.bin" ^
    0x20000 "%BUILD_DIR%\furi_esp32.bin" || exit /b 1

echo.
echo Built: %BUILD_DIR%\%OUTNAME%
echo   Firmware only. Extract sdcard.zip to the SD card by hand.

if not defined PORT goto :done
echo.
echo === flashing %PORT% ===
python -m esptool --chip esp32s3 -p %PORT% -b 460800 ^
    --before default_reset --after hard_reset ^
    write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB ^
    0x0 "%BUILD_DIR%\%OUTNAME%" || exit /b 1

:done
endlocal
