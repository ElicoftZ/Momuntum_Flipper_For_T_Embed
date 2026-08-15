#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <core/common_defines.h>
#include <datetime/datetime.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FuriHalRtcFlagDebug = (1 << 0),
    FuriHalRtcFlagStorageFormatInternal = (1 << 1),
    FuriHalRtcFlagLock = (1 << 2),
    FuriHalRtcFlagC2Update = (1 << 3),
    FuriHalRtcFlagHandOrient = (1 << 4),
    FuriHalRtcFlagLegacySleep = (1 << 5),
    FuriHalRtcFlagStealthMode = (1 << 6),
    FuriHalRtcFlagDetailedFilename = (1 << 7),
} FuriHalRtcFlag;

typedef enum {
    FuriHalRtcBootModeNormal = 0,
    FuriHalRtcBootModeDfu,
    FuriHalRtcBootModePreUpdate,
    FuriHalRtcBootModeUpdate,
    FuriHalRtcBootModePostUpdate,
} FuriHalRtcBootMode;

typedef enum {
    FuriHalRtcHeapTrackModeNone = 0,
    FuriHalRtcHeapTrackModeMain = 1,
    FuriHalRtcHeapTrackModeTree = 2,
    FuriHalRtcHeapTrackModeAll = 3,
} FuriHalRtcHeapTrackMode;

typedef enum {
    FuriHalRtcLogDeviceUsart = 0x0,
    FuriHalRtcLogDeviceLpuart = 0x1,
    FuriHalRtcLogDeviceReserved = 0x2,
    FuriHalRtcLogDeviceNone = 0x3,
} FuriHalRtcLogDevice;

typedef enum {
    FuriHalRtcLogBaudRate230400 = 0x0,
    FuriHalRtcLogBaudRate9600 = 0x1,
    FuriHalRtcLogBaudRate38400 = 0x2,
    FuriHalRtcLogBaudRate57600 = 0x3,
    FuriHalRtcLogBaudRate115200 = 0x4,
    FuriHalRtcLogBaudRate460800 = 0x5,
    FuriHalRtcLogBaudRate921600 = 0x6,
    FuriHalRtcLogBaudRate1843200 = 0x7,
} FuriHalRtcLogBaudRate;

typedef enum {
    FuriHalRtcLocaleUnitsMetric = 0x0,
    FuriHalRtcLocaleUnitsImperial = 0x1,
} FuriHalRtcLocaleUnits;

typedef enum {
    FuriHalRtcLocaleTimeFormat24h = 0x0,
    FuriHalRtcLocaleTimeFormat12h = 0x1,
} FuriHalRtcLocaleTimeFormat;

typedef enum {
    FuriHalRtcLocaleDateFormatDMY = 0x0,
    FuriHalRtcLocaleDateFormatMDY = 0x1,
    FuriHalRtcLocaleDateFormatYMD = 0x2,
} FuriHalRtcLocaleDateFormat;

void furi_hal_rtc_init_early(void);
void furi_hal_rtc_deinit_early(void);
void furi_hal_rtc_init(void);
void furi_hal_rtc_prepare_for_shutdown(void);
void furi_hal_rtc_sync_shadow(void);
void furi_hal_rtc_reset_registers(void);

void furi_hal_rtc_set_log_level(uint8_t level);
uint8_t furi_hal_rtc_get_log_level(void);
void furi_hal_rtc_set_log_device(FuriHalRtcLogDevice device);
FuriHalRtcLogDevice furi_hal_rtc_get_log_device(void);
void furi_hal_rtc_set_log_baud_rate(FuriHalRtcLogBaudRate baud_rate);
FuriHalRtcLogBaudRate furi_hal_rtc_get_log_baud_rate(void);

void furi_hal_rtc_set_fault_data(uint32_t value);
uint32_t furi_hal_rtc_get_fault_data(void);
void furi_hal_rtc_set_pin_fails(uint32_t value);
uint32_t furi_hal_rtc_get_pin_fails(void);
void furi_hal_rtc_set_pin_value(uint32_t value);
uint32_t furi_hal_rtc_get_pin_value(void);

bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag);
void furi_hal_rtc_set_flag(FuriHalRtcFlag flag);
void furi_hal_rtc_reset_flag(FuriHalRtcFlag flag);

FuriHalRtcBootMode furi_hal_rtc_get_boot_mode(void);
void furi_hal_rtc_set_boot_mode(FuriHalRtcBootMode mode);
FuriHalRtcHeapTrackMode furi_hal_rtc_get_heap_track_mode(void);
void furi_hal_rtc_set_heap_track_mode(FuriHalRtcHeapTrackMode mode);

void furi_hal_rtc_get_datetime(DateTime* datetime);
void furi_hal_rtc_set_datetime(DateTime* datetime);
uint32_t furi_hal_rtc_get_timestamp(void);

FuriHalRtcLocaleTimeFormat furi_hal_rtc_get_locale_timeformat(void);
void furi_hal_rtc_set_locale_timeformat(FuriHalRtcLocaleTimeFormat format);
FuriHalRtcLocaleDateFormat furi_hal_rtc_get_locale_dateformat(void);
void furi_hal_rtc_set_locale_dateformat(FuriHalRtcLocaleDateFormat format);
FuriHalRtcLocaleUnits furi_hal_rtc_get_locale_units(void);
void furi_hal_rtc_set_locale_units(FuriHalRtcLocaleUnits format);

/** Maximum length of a stored resume target, including the terminator. */
#define FURI_HAL_RTC_RESUME_APP_SIZE 64

/** Remember which app to come back to after deep sleep.
 *
 * Held in RTC memory, which survives deep sleep; the rest of this HAL's state
 * is ordinary RAM and does not. Pass NULL to forget the current target.
 */
void furi_hal_rtc_set_resume_app(const char* name);

/** Mark the stored target as one that should actually be resumed.
 *
 * Idle sleep and a deliberate power off both end in the same deep sleep, so
 * they are indistinguishable at wake. Only the idle path arms the resume.
 */
void furi_hal_rtc_arm_resume(void);

/** Take the resume target, if this boot woke from an armed deep sleep.
 *
 * Consumes it: a later call returns false until something is stored and armed
 * again, so a resume cannot repeat across an ordinary reboot.
 */
bool furi_hal_rtc_take_resume_app(char* name, size_t name_size);

#ifdef __cplusplus
}
#endif
