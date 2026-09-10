@echo off
setlocal EnableExtensions
rem ---------------------------------------------------------------------------
rem Build one of the T-Embed CC1101 variants.
rem
rem Run with no arguments for a menu. Passing the name still works and stays
rem non-interactive, so scripts and CI are unaffected.
rem
rem   build_variant.bat dualboot  [COMx]  -- protected Flipper + replaceable firmware
rem   build_variant.bat release   [COMx]  -- public build, no dual boot
rem
rem They differ in the partition table and in whether the Dual Boot app is built
rem at all, so each gets its OWN build directory. Sharing one would silently
rem reuse the cached sdkconfig and produce the wrong partition layout -- which
rem is exactly what happened when this was one directory.
rem ---------------------------------------------------------------------------

cd /d "%~dp0"

set "VARIANT=%~1"
set "PORT=%~2"

rem No variant on the command line: ask. Typing a name works as well as a number,
rem and either way it falls through to the same validation below.
if not "%VARIANT%"=="" goto :chosen
echo.
echo   Which build?
echo.
echo     1. dualboot    protected Flipper + replaceable 8 MiB firmware slot
echo     2. release     public build, no dual boot
echo.
set "CHOICE="
set /p "CHOICE=Enter 1-2, or a name: "
if "%CHOICE%"=="1" set "VARIANT=dualboot"
if "%CHOICE%"=="2" set "VARIANT=release"
rem Anything else is treated as a name so a typo lands on the usage message
rem rather than silently building the wrong thing.
if "%VARIANT%"=="" set "VARIANT=%CHOICE%"
if "%VARIANT%"=="" (
    echo Nothing selected.
    exit /b 1
)
echo.
:chosen

if /i "%VARIANT%"=="dualboot"  goto :ok
if /i "%VARIANT%"=="release"   goto :ok
echo Usage: build_variant.bat ^<dualboot^|release^> [COMx]
exit /b 1
:ok

rem FRAGMENT empty means "use the root sdkconfig untouched", which is what dualboot
rem does. Any variant that sets one gets its own sdkconfig inside its build dir.
set "FRAGMENT="

if /i "%VARIANT%"=="dualboot" (
    set "BUILD_DIR=build_t_embed"
    set "TABLE=partitions_singleapp_16mb.csv"
    set "OUTNAME=momentum_t_embed_DUALBOOT.bin"
    set "MOMENTUM_RELEASE_BUILD="
)
if /i "%VARIANT%"=="release" (
    set "BUILD_DIR=build_t_embed_release"
    set "TABLE=partitions_release_16mb.csv"
    set "OUTNAME=momentum_t_embed_RELEASE.bin"
    set "FRAGMENT=sdkconfig.variant_release"
    rem Excludes the Dual Boot app from the build entirely -- for a public image
    rem "hidden" is not enough, it should not be on the device at all.
    rem fam_config.py reads this from the environment, not from a CMake define.
    set "MOMENTUM_RELEASE_BUILD=1"
)

set "IDF_ROOT=%ESP_IDF_DIR%"
if not defined IDF_ROOT set "IDF_ROOT=C:\Espressif\frameworks\esp-idf-v5.4.1"
if not defined IDF_TOOLS_PATH for %%I in ("%IDF_ROOT%\..\..") do set "IDF_TOOLS_PATH=%%~fI"

rem Pick the ESP-IDF python env DETERMINISTICALLY, in preference order.
rem
rem The old glob was `for /d ... idf5.4_py*`, which keeps whichever directory
rem sorts LAST -- so with both idf5.4_py3.11_env and idf5.4_py3.12_env installed
rem it silently chose 3.12, while winbuild.py and the documented build recipe
rem use 3.11. A build directory records the interpreter it was configured with
rem and refuses to build under a different one, so the two paths fought over
rem build_t_embed and idf.py demanded a fullclean. Pin the order instead.
set "IDF_PYTHON_DIR="
for %%V in (3.11 3.12 3.13) do (
    if not defined IDF_PYTHON_DIR (
        if exist "%IDF_TOOLS_PATH%\python_env\idf5.4_py%%V_env\Scripts\python.exe" (
            set "IDF_PYTHON_DIR=%IDF_TOOLS_PATH%\python_env\idf5.4_py%%V_env\Scripts"
        )
    )
)
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
if not "%FRAGMENT%"=="" (
    idf.py -B "%BUILD_DIR%" ^
        -DFLIPPER_BOARD=%FLIPPER_BOARD% ^
        -DSDKCONFIG="%BUILD_DIR%\sdkconfig" ^
        -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;%FRAGMENT%" ^
        build || exit /b 1
) else (
    idf.py -B "%BUILD_DIR%" ^
        -DFLIPPER_BOARD=%FLIPPER_BOARD% ^
        build || exit /b 1
)

rem Prove the release image really got the release table. The bug this guards
rem against was completely silent: the build went green while using the
rem dual-boot table, and nothing downstream complained. ota_0 exists only in
rem the dual-boot table, so finding it here means the wrong table is in force.
if /i not "%VARIANT%"=="release" goto :skip_table_check
python "%IDF_ROOT%\components\partition_table\gen_esp32part.py" ^
    "%BUILD_DIR%\partition_table\partition-table.bin" > "%BUILD_DIR%\partition_table_dump.txt"
findstr /b /c:"ota_0," "%BUILD_DIR%\partition_table_dump.txt" >nul
if not errorlevel 1 (
    echo.
    echo ERROR: release build has an 'ota_0' partition - dual-boot table in use.
    echo        Check sdkconfig.variant_release and %BUILD_DIR%\sdkconfig.
    type "%BUILD_DIR%\partition_table_dump.txt"
    exit /b 1
)
:skip_table_check

rem No assets.tar is packed or merged any more: the release table has no
rem `assets` partition, so the merged image is just bootloader + table + app and
rem lands at ~3.3 MB instead of 8.5 MB of mostly 0xFF padding. That size is what
rem the web flasher pushes over USB-CDC, so it is the difference between a quick
rem flash and a flaky one. SD content ships in the release zip instead.
python -m esptool --chip esp32s3 merge_bin -o "%BUILD_DIR%\%OUTNAME%" ^
    --flash_mode dio --flash_freq 80m --flash_size 16MB ^
    0x0 "%BUILD_DIR%\bootloader\bootloader.bin" ^
    0x8000 "%BUILD_DIR%\partition_table\partition-table.bin" ^
    0x20000 "%BUILD_DIR%\furi_esp32.bin" || exit /b 1

echo.
echo Built: %BUILD_DIR%\%OUTNAME%
if /i "%VARIANT%"=="dualboot"  echo   Secondary 8 MiB slot starts at 0x720000; manage it from Dual Boot 2.0.
if /i "%VARIANT%"=="release"   echo   Firmware only; SD content ships separately in the release zip.

if not "%PORT%"=="" (
    python -m esptool --chip esp32s3 -p "%PORT%" -b 460800 --before default_reset --after hard_reset ^
        write_flash --flash_size 16MB 0x0 "%BUILD_DIR%\%OUTNAME%" || exit /b 1
)

endlocal
