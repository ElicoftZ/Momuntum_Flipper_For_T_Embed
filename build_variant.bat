@echo off
setlocal EnableExtensions
rem ---------------------------------------------------------------------------
rem Build one of the two T-Embed CC1101 variants.
rem
rem   build_variant.bat dualboot [COMx]   -- personal build, keeps the Bruce slot
rem   build_variant.bat release  [COMx]   -- public build, no dual boot
rem
rem They differ in the partition table and in whether the Dual Boot app is built
rem at all, so each gets its OWN build directory. Sharing one would silently
rem reuse the cached sdkconfig and produce the wrong partition layout -- which
rem is exactly what happened when this was one directory.
rem ---------------------------------------------------------------------------

cd /d "%~dp0"

set "VARIANT=%~1"
set "PORT=%~2"

if /i "%VARIANT%"=="dualboot" goto :ok
if /i "%VARIANT%"=="release"  goto :ok
echo Usage: build_variant.bat ^<dualboot^|release^> [COMx]
exit /b 1
:ok

if /i "%VARIANT%"=="dualboot" (
    set "BUILD_DIR=build_t_embed"
    set "TABLE=partitions_singleapp_16mb.csv"
    set "OUTNAME=momentum_t_embed_DUALBOOT.bin"
    set "MOMENTUM_RELEASE_BUILD="
) else (
    set "BUILD_DIR=build_t_embed_release"
    set "TABLE=partitions_release_16mb.csv"
    set "OUTNAME=momentum_t_embed_RELEASE.bin"
    rem Excludes the Dual Boot app from the build entirely -- for a public image
    rem "hidden" is not enough, it should not be on the device at all.
    rem fam_config.py reads this from the environment, not from a CMake define.
    set "MOMENTUM_RELEASE_BUILD=1"
)

set "IDF_ROOT=%ESP_IDF_DIR%"
if not defined IDF_ROOT set "IDF_ROOT=C:\Espressif\frameworks\esp-idf-v5.4.1"
if not defined IDF_TOOLS_PATH for %%I in ("%IDF_ROOT%\..\..") do set "IDF_TOOLS_PATH=%%~fI"

set "IDF_PYTHON_DIR="
for /d %%P in ("%IDF_TOOLS_PATH%\python_env\idf5.4_py*") do if exist "%%~fP\Scripts\python.exe" set "IDF_PYTHON_DIR=%%~fP\Scripts"
if defined IDF_PYTHON_DIR set "PATH=%IDF_PYTHON_DIR%;%PATH%"

call "%IDF_ROOT%\export.bat" >nul || (echo ERROR: ESP-IDF export failed & exit /b 1)

set "FLIPPER_BOARD=lilygo_t_embed_cc1101"


echo.
echo === %VARIANT% : table %TABLE% -^> %BUILD_DIR% ===
echo.

rem The release variant gets its OWN sdkconfig, inside its build dir. ESP-IDF
rem keeps sdkconfig in the PROJECT ROOT by default, so both variants otherwise
rem share one config file and the separate build dirs isolate nothing -- which
rem is how the release build silently inherited the dual-boot 5 MB table and
rem came out with no `assets` partition at all.
rem
rem The table is a Kconfig option, so it has to arrive through a defaults
rem fragment. The `-DPARTITION_TABLE_CUSTOM_FILENAME=` that used to sit here set
rem an unrelated CMake cache variable that nothing reads, so it never had any
rem effect at all.
rem
rem dualboot deliberately still uses the root sdkconfig, untouched, so this
rem cannot disturb a build that is already working.
if /i "%VARIANT%"=="release" (
    idf.py -B "%BUILD_DIR%" ^
        -DFLIPPER_BOARD=%FLIPPER_BOARD% ^
        -DSDKCONFIG="%BUILD_DIR%\sdkconfig" ^
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.variant_release" ^
        build || exit /b 1
) else (
    idf.py -B "%BUILD_DIR%" ^
        -DFLIPPER_BOARD=%FLIPPER_BOARD% ^
        build || exit /b 1
)

rem Prove the release image really got the release table before packing anything
rem into it. The bug this guards against was completely silent: the build went
rem green, and only the boot log would have shown the installer finding no
rem partition to read.
if /i not "%VARIANT%"=="release" goto :skip_table_check
python "%IDF_ROOT%\components\partition_table\gen_esp32part.py" ^
    "%BUILD_DIR%\partition_table\partition-table.bin" > "%BUILD_DIR%\partition_table_dump.txt"
findstr /b /c:"assets," "%BUILD_DIR%\partition_table_dump.txt" >nul
if errorlevel 1 (
    echo.
    echo ERROR: release build has no 'assets' partition - wrong table in use.
    echo        Check sdkconfig.variant_release and %BUILD_DIR%\sdkconfig.
    type "%BUILD_DIR%\partition_table_dump.txt"
    exit /b 1
)
:skip_table_check

rem Pack the SD delta for the release image. Harmless for dualboot, but only the
rem release layout has an `assets` partition to hold it.
if /i "%VARIANT%"=="release" (
    python "%~dp0tools\pack_assets.py" --out "%BUILD_DIR%\assets.tar" || exit /b 1
)

rem The release image carries the SD delta at the assets partition offset, so
rem one flash gives both the firmware and the content it installs on first boot.
set "ASSETS_ARG="
if /i "%VARIANT%"=="release" set ASSETS_ARG=0x820000 "%BUILD_DIR%\assets.tar"
python -m esptool --chip esp32s3 merge_bin -o "%BUILD_DIR%\%OUTNAME%" ^
    --flash_mode dio --flash_freq 80m --flash_size 16MB ^
    0x0 "%BUILD_DIR%\bootloader\bootloader.bin" ^
    0x8000 "%BUILD_DIR%\partition_table\partition-table.bin" ^
    0x20000 "%BUILD_DIR%\furi_esp32.bin" %ASSETS_ARG% || exit /b 1

echo.
echo Built: %BUILD_DIR%\%OUTNAME%
if /i "%VARIANT%"=="dualboot" echo   Bruce slot at 0x520000 is preserved; flash Bruce separately.
if /i "%VARIANT%"=="release"  echo   assets.tar is merged at 0x820000; first boot installs it to a fresh SD card.

if not "%PORT%"=="" (
    python -m esptool --chip esp32s3 -p "%PORT%" -b 460800 --before default_reset --after hard_reset ^
        write_flash --flash_size 16MB 0x0 "%BUILD_DIR%\%OUTNAME%" || exit /b 1
)

endlocal
