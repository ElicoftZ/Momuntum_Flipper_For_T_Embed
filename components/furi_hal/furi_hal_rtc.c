#include "furi_hal_rtc.h"

#include <esp_attr.h>
#include <esp_private/esp_clk.h>
#include <esp_sleep.h>
#include <sdkconfig.h>
#include <soc/rtc.h>
#include <toolbox/saved_struct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FURI_HAL_RTC_LOCALE_PATH "/int/.locale.settings"
#define FURI_HAL_RTC_LOCALE_MAGIC 0x4C
#define FURI_HAL_RTC_LOCALE_VERSION 2
#define FURI_HAL_RTC_LOCALE_VERSION_LEGACY 1
#define FURI_HAL_RTC_TIMEZONE_MINUTES_MIN (-12 * 60)
#define FURI_HAL_RTC_TIMEZONE_MINUTES_MAX (14 * 60)

typedef struct {
    uint8_t time_format;
    uint8_t date_format;
    uint8_t units;
} FuriHalRtcLocaleSettingsLegacy;

typedef struct {
    int16_t timezone_offset_minutes;
    uint8_t time_format;
    uint8_t date_format;
    uint8_t units;
    uint8_t timezone_auto;
} FuriHalRtcLocaleSettings;

typedef struct {
    FuriHalRtcHeapTrackMode heap_track_mode;
    FuriHalRtcBootMode boot_mode;
    FuriHalRtcLogDevice log_device;
    FuriHalRtcLogBaudRate log_baud_rate;
    uint8_t log_level;
    uint32_t flags;
    uint32_t fault_data;
    uint32_t pin_fails;
    uint32_t pin_value;
    int64_t time_offset;
    FuriHalRtcLocaleTimeFormat locale_timeformat;
    FuriHalRtcLocaleDateFormat locale_dateformat;
    FuriHalRtcLocaleUnits locale_units;
    int16_t timezone_offset_minutes;
    bool timezone_auto;
} FuriHalRtcState;

static FuriHalRtcState furi_hal_rtc = {
    .heap_track_mode = FuriHalRtcHeapTrackModeNone,
    .boot_mode = FuriHalRtcBootModeNormal,
    .log_device = FuriHalRtcLogDeviceNone,
    .log_baud_rate = FuriHalRtcLogBaudRate115200,
    .log_level = 0,
    .flags = 0,
    .fault_data = 0,
    .pin_fails = 0,
    .pin_value = 0,
    .time_offset = 0,
    .locale_timeformat = FuriHalRtcLocaleTimeFormat24h,
    .locale_dateformat = FuriHalRtcLocaleDateFormatDMY,
    .locale_units = FuriHalRtcLocaleUnitsMetric,
    .timezone_offset_minutes = 0,
    .timezone_auto = true,
};

static bool furi_hal_rtc_timezone_offset_is_valid(int16_t offset_minutes) {
    return offset_minutes >= FURI_HAL_RTC_TIMEZONE_MINUTES_MIN &&
           offset_minutes <= FURI_HAL_RTC_TIMEZONE_MINUTES_MAX;
}

static void furi_hal_rtc_apply_timezone(void) {
    const int offset_minutes = furi_hal_rtc.timezone_offset_minutes;
    char timezone[16];

    if(offset_minutes == 0) {
        strlcpy(timezone, "UTC0", sizeof(timezone));
    } else {
        /* POSIX TZ signs are intentionally reversed: UTC-06:00 local time is
         * written as UTC+06:00 because the value is added to local to get UTC. */
        const int absolute_minutes = abs(offset_minutes);
        snprintf(
            timezone,
            sizeof(timezone),
            "UTC%c%02d:%02d",
            offset_minutes > 0 ? '-' : '+',
            absolute_minutes / 60,
            absolute_minutes % 60);
    }

    if(setenv("TZ", timezone, 1) == 0) tzset();
}

static void furi_hal_rtc_save_locale(void) {
    const FuriHalRtcLocaleSettings settings = {
        .timezone_offset_minutes = furi_hal_rtc.timezone_offset_minutes,
        .time_format = (uint8_t)furi_hal_rtc.locale_timeformat,
        .date_format = (uint8_t)furi_hal_rtc.locale_dateformat,
        .units = (uint8_t)furi_hal_rtc.locale_units,
        .timezone_auto = furi_hal_rtc.timezone_auto ? 1U : 0U,
    };
    saved_struct_save(
        FURI_HAL_RTC_LOCALE_PATH,
        &settings,
        sizeof(settings),
        FURI_HAL_RTC_LOCALE_MAGIC,
        FURI_HAL_RTC_LOCALE_VERSION);
}

static void furi_hal_rtc_load_locale(void) {
    FuriHalRtcLocaleSettings settings;
    if(!saved_struct_load(
           FURI_HAL_RTC_LOCALE_PATH,
           &settings,
           sizeof(settings),
           FURI_HAL_RTC_LOCALE_MAGIC,
           FURI_HAL_RTC_LOCALE_VERSION)) {
        /* Version 1 only held the three upstream locale fields. Preserve them
         * during upgrade, then write the extended release-safe version. */
        FuriHalRtcLocaleSettingsLegacy legacy;
        if(saved_struct_load(
               FURI_HAL_RTC_LOCALE_PATH,
               &legacy,
               sizeof(legacy),
               FURI_HAL_RTC_LOCALE_MAGIC,
               FURI_HAL_RTC_LOCALE_VERSION_LEGACY)) {
            settings.time_format = legacy.time_format;
            settings.date_format = legacy.date_format;
            settings.units = legacy.units;
            settings.timezone_offset_minutes = 0;
            settings.timezone_auto = 1U;
        } else {
            furi_hal_rtc_save_locale();
            return;
        }
    }

    if(settings.time_format <= FuriHalRtcLocaleTimeFormat12h) {
        furi_hal_rtc.locale_timeformat = (FuriHalRtcLocaleTimeFormat)settings.time_format;
    }
    if(settings.date_format <= FuriHalRtcLocaleDateFormatYMD) {
        furi_hal_rtc.locale_dateformat = (FuriHalRtcLocaleDateFormat)settings.date_format;
    }
    if(settings.units <= FuriHalRtcLocaleUnitsImperial) {
        furi_hal_rtc.locale_units = (FuriHalRtcLocaleUnits)settings.units;
    }
    if(furi_hal_rtc_timezone_offset_is_valid(settings.timezone_offset_minutes)) {
        furi_hal_rtc.timezone_offset_minutes = settings.timezone_offset_minutes;
    }
    furi_hal_rtc.timezone_auto = settings.timezone_auto != 0U;
    furi_hal_rtc_save_locale();
}

/* RTC_NOINIT_ATTR lives in RTC slow memory, which survives deep sleep but holds
 * whatever was there on a cold boot, so the magic guards against garbage. */
#define FURI_HAL_RTC_RESUME_MAGIC 0x52534d31UL // "RSM1"

RTC_NOINIT_ATTR static uint32_t furi_hal_rtc_resume_magic;
RTC_NOINIT_ATTR static uint32_t furi_hal_rtc_resume_armed;
RTC_NOINIT_ATTR static char furi_hal_rtc_resume_app[FURI_HAL_RTC_RESUME_APP_SIZE];

/* Build a monotonic elapsed clock directly from raw RTC slow-clock ticks. The
 * ESP-IDF absolute RTC-time helper can rebase its retained origin after a new
 * image is flashed, producing a large one-time jump. Accumulating tick deltas
 * with the calibration that was valid when each interval began avoids that
 * jump while remaining completely independent of calendar/SNTP time. */
#define FURI_HAL_RTC_ELAPSED_MAGIC 0x454c5031UL // "ELP1"
RTC_NOINIT_ATTR static uint32_t furi_hal_rtc_elapsed_magic;
RTC_NOINIT_ATTR static uint32_t furi_hal_rtc_elapsed_cal;
RTC_NOINIT_ATTR static uint64_t furi_hal_rtc_elapsed_last_ticks;
RTC_NOINIT_ATTR static uint64_t furi_hal_rtc_elapsed_total_us;

/* Deep-sleep entry and the completed sleep duration also live in RTC slow
 * memory, so Clock reports only time when the device was truly powered down. */
#define FURI_HAL_RTC_INACTIVITY_MAGIC 0x494e4133UL // "INA3"
RTC_NOINIT_ATTR static uint32_t furi_hal_rtc_inactivity_magic;
RTC_NOINIT_ATTR static uint32_t furi_hal_rtc_sleep_armed;
RTC_NOINIT_ATTR static uint64_t furi_hal_rtc_sleep_start_us;
RTC_NOINIT_ATTR static uint64_t furi_hal_rtc_last_sleep_seconds;

static uint64_t furi_hal_rtc_slowclk_to_us(uint64_t ticks, uint32_t period) {
    /* Split the multiplication like ESP-IDF's esp_rtc_get_time_us() so the
     * 48-bit RTC counter cannot overflow a 64-bit intermediate. */
    const uint64_t ticks_low = ticks & UINT32_MAX;
    const uint64_t ticks_high = ticks >> 32;
    return ((ticks_low * period) >> RTC_CLK_CAL_FRACT) +
           ((ticks_high * period) << (32 - RTC_CLK_CAL_FRACT));
}

/* Caller holds the Furi critical section. */
static uint64_t furi_hal_rtc_counter_update_us(void) {
    const uint64_t now_ticks = rtc_time_get();
    const uint32_t current_cal = esp_clk_slowclk_cal_get();
    const bool retained_state_valid =
        furi_hal_rtc_elapsed_magic == FURI_HAL_RTC_ELAPSED_MAGIC &&
        furi_hal_rtc_elapsed_cal != 0 && current_cal != 0 &&
        furi_hal_rtc_elapsed_last_ticks <= now_ticks;

    if(!retained_state_valid) {
        furi_hal_rtc_elapsed_magic = FURI_HAL_RTC_ELAPSED_MAGIC;
        furi_hal_rtc_elapsed_cal = current_cal;
        furi_hal_rtc_elapsed_last_ticks = now_ticks;
        furi_hal_rtc_elapsed_total_us = 0;
        return 0;
    }

    const uint64_t delta_ticks = now_ticks - furi_hal_rtc_elapsed_last_ticks;
    furi_hal_rtc_elapsed_total_us +=
        furi_hal_rtc_slowclk_to_us(delta_ticks, furi_hal_rtc_elapsed_cal);
    furi_hal_rtc_elapsed_last_ticks = now_ticks;
    furi_hal_rtc_elapsed_cal = current_cal;
    return furi_hal_rtc_elapsed_total_us;
}

static void furi_hal_rtc_calibrate_slow_clock(void) {
#if CONFIG_RTC_CLK_CAL_CYCLES > 0
    /* The calibration value is retained by ESP-IDF across warm resets and can
     * be stale or invalid after an app-only reflash. Recalibrate the selected
     * RTC source on every boot so both elapsed conversion and timer wakeups use
     * the real slow-clock period. */
    const uint32_t calibration =
        rtc_clk_cal(RTC_CAL_RTC_MUX, CONFIG_RTC_CLK_CAL_CYCLES);
    if(calibration != 0) esp_clk_slowclk_cal_set(calibration);
#endif
}

static void furi_hal_rtc_inactivity_init(void) {
    FURI_CRITICAL_ENTER();
    const uint64_t now_us = furi_hal_rtc_counter_update_us();
    const bool retained_state_valid =
        furi_hal_rtc_inactivity_magic == FURI_HAL_RTC_INACTIVITY_MAGIC &&
        furi_hal_rtc_sleep_start_us <= now_us &&
        furi_hal_rtc_last_sleep_seconds <= now_us / 1000000ULL;

    if(!retained_state_valid) {
        furi_hal_rtc_inactivity_magic = FURI_HAL_RTC_INACTIVITY_MAGIC;
        furi_hal_rtc_sleep_armed = 0;
        furi_hal_rtc_sleep_start_us = now_us;
        furi_hal_rtc_last_sleep_seconds = 0;
        FURI_CRITICAL_EXIT();
        return;
    }

    const bool woke_from_deep_sleep =
        esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
    if(woke_from_deep_sleep && furi_hal_rtc_sleep_armed == 1U) {
        furi_hal_rtc_last_sleep_seconds =
            (now_us - furi_hal_rtc_sleep_start_us) / 1000000ULL;
    }

    /* Consume the marker even on an unrelated reset. A later reboot must not
     * misinterpret an abandoned shutdown attempt as a completed sleep. */
    furi_hal_rtc_sleep_armed = 0;
    FURI_CRITICAL_EXIT();
}

void furi_hal_rtc_set_resume_app(const char* name) {
    if(name && name[0]) {
        strlcpy(furi_hal_rtc_resume_app, name, sizeof(furi_hal_rtc_resume_app));
        furi_hal_rtc_resume_magic = FURI_HAL_RTC_RESUME_MAGIC;
    } else {
        furi_hal_rtc_resume_app[0] = '\0';
        furi_hal_rtc_resume_magic = 0;
    }
    /* Storing a new target invalidates any earlier arming. */
    furi_hal_rtc_resume_armed = 0;
}

void furi_hal_rtc_arm_resume(void) {
    if(furi_hal_rtc_resume_magic == FURI_HAL_RTC_RESUME_MAGIC) {
        furi_hal_rtc_resume_armed = 1;
    }
}

bool furi_hal_rtc_take_resume_app(char* name, size_t name_size) {
    if(!name || name_size == 0) return false;

    const bool woke_from_deep_sleep =
        esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_UNDEFINED;
    const bool valid = furi_hal_rtc_resume_magic == FURI_HAL_RTC_RESUME_MAGIC &&
                       furi_hal_rtc_resume_armed == 1 && furi_hal_rtc_resume_app[0] != '\0';

    /* Consume regardless, so a stale target cannot fire on a later boot. */
    furi_hal_rtc_resume_armed = 0;

    if(!woke_from_deep_sleep || !valid) {
        if(!woke_from_deep_sleep) {
            furi_hal_rtc_resume_magic = 0;
            furi_hal_rtc_resume_app[0] = '\0';
        }
        return false;
    }

    strlcpy(name, furi_hal_rtc_resume_app, name_size);
    return true;
}

static time_t furi_hal_rtc_now(void) {
    return time(NULL) + (time_t)furi_hal_rtc.time_offset;
}

void furi_hal_rtc_init_early(void) {
    furi_hal_rtc_calibrate_slow_clock();
    furi_hal_rtc_inactivity_init();
}

void furi_hal_rtc_deinit_early(void) {
}

void furi_hal_rtc_init(void) {
    /* ESP32 has no Flipper backup registers for locale preferences. NVS is
     * initialized immediately before this call, so restore their equivalent. */
    furi_hal_rtc_load_locale();
    furi_hal_rtc_apply_timezone();
}

void furi_hal_rtc_prepare_for_shutdown(void) {
    FURI_CRITICAL_ENTER();
    const uint64_t now_us = furi_hal_rtc_counter_update_us();
    furi_hal_rtc_inactivity_magic = FURI_HAL_RTC_INACTIVITY_MAGIC;
    furi_hal_rtc_sleep_start_us = now_us;
    furi_hal_rtc_sleep_armed = 1U;
    FURI_CRITICAL_EXIT();
}

void furi_hal_rtc_sync_shadow(void) {
}

void furi_hal_rtc_reset_registers(void) {
    furi_hal_rtc.flags = 0;
    furi_hal_rtc.fault_data = 0;
    furi_hal_rtc.pin_fails = 0;
    furi_hal_rtc.pin_value = 0;
    furi_hal_rtc.boot_mode = FuriHalRtcBootModeNormal;
}

void furi_hal_rtc_set_log_level(uint8_t level) {
    furi_hal_rtc.log_level = level;
}

uint8_t furi_hal_rtc_get_log_level(void) {
    return furi_hal_rtc.log_level;
}

void furi_hal_rtc_set_log_device(FuriHalRtcLogDevice device) {
    furi_hal_rtc.log_device = device;
}

FuriHalRtcLogDevice furi_hal_rtc_get_log_device(void) {
    return furi_hal_rtc.log_device;
}

void furi_hal_rtc_set_log_baud_rate(FuriHalRtcLogBaudRate baud_rate) {
    furi_hal_rtc.log_baud_rate = baud_rate;
}

FuriHalRtcLogBaudRate furi_hal_rtc_get_log_baud_rate(void) {
    return furi_hal_rtc.log_baud_rate;
}

void furi_hal_rtc_set_fault_data(uint32_t value) {
    furi_hal_rtc.fault_data = value;
}

uint32_t furi_hal_rtc_get_fault_data(void) {
    return furi_hal_rtc.fault_data;
}

void furi_hal_rtc_set_pin_fails(uint32_t value) {
    furi_hal_rtc.pin_fails = value;
}

uint32_t furi_hal_rtc_get_pin_fails(void) {
    return furi_hal_rtc.pin_fails;
}

void furi_hal_rtc_set_pin_value(uint32_t value) {
    furi_hal_rtc.pin_value = value;
}

uint32_t furi_hal_rtc_get_pin_value(void) {
    return furi_hal_rtc.pin_value;
}

bool furi_hal_rtc_is_flag_set(FuriHalRtcFlag flag) {
    return (furi_hal_rtc.flags & flag) != 0;
}

void furi_hal_rtc_set_flag(FuriHalRtcFlag flag) {
    furi_hal_rtc.flags |= flag;
}

void furi_hal_rtc_reset_flag(FuriHalRtcFlag flag) {
    furi_hal_rtc.flags &= ~flag;
}

FuriHalRtcBootMode furi_hal_rtc_get_boot_mode(void) {
    return furi_hal_rtc.boot_mode;
}

void furi_hal_rtc_set_boot_mode(FuriHalRtcBootMode mode) {
    furi_hal_rtc.boot_mode = mode;
}

FuriHalRtcHeapTrackMode furi_hal_rtc_get_heap_track_mode(void) {
    return furi_hal_rtc.heap_track_mode;
}

void furi_hal_rtc_set_heap_track_mode(FuriHalRtcHeapTrackMode mode) {
    furi_hal_rtc.heap_track_mode = mode;
}

void furi_hal_rtc_get_datetime(DateTime* datetime) {
    if(!datetime) {
        return;
    }

    time_t now = furi_hal_rtc_now();
    struct tm now_tm = {0};
    localtime_r(&now, &now_tm);

    datetime->hour = now_tm.tm_hour;
    datetime->minute = now_tm.tm_min;
    datetime->second = now_tm.tm_sec;
    datetime->day = now_tm.tm_mday;
    datetime->month = now_tm.tm_mon + 1;
    datetime->year = now_tm.tm_year + 1900;
    datetime->weekday = ((now_tm.tm_wday + 6) % 7) + 1;
}

void furi_hal_rtc_set_datetime(DateTime* datetime) {
    if(!datetime) {
        return;
    }

    struct tm desired = {
        .tm_sec = datetime->second,
        .tm_min = datetime->minute,
        .tm_hour = datetime->hour,
        .tm_mday = datetime->day,
        .tm_mon = datetime->month - 1,
        .tm_year = datetime->year - 1900,
        .tm_isdst = -1,
    };

    const time_t target = mktime(&desired);
    if(target != (time_t)-1) {
        furi_hal_rtc.time_offset = (int64_t)target - (int64_t)time(NULL);
    }
}

void furi_hal_rtc_sync_system_time(void) {
    /* SNTP updates time(NULL) directly. A date previously entered through the
     * Flipper API is represented as an offset on top of time(NULL), so keeping
     * that offset would make the freshly synchronized clock wrong by the old
     * correction. */
    furi_hal_rtc.time_offset = 0;
}

uint32_t furi_hal_rtc_get_timestamp(void) {
    return (uint32_t)furi_hal_rtc_now();
}

uint64_t furi_hal_rtc_get_counter_seconds(void) {
    FURI_CRITICAL_ENTER();
    const uint64_t elapsed = furi_hal_rtc_counter_update_us() / 1000000ULL;
    FURI_CRITICAL_EXIT();
    return elapsed;
}

uint64_t furi_hal_rtc_get_inactivity_seconds(void) {
    FURI_CRITICAL_ENTER();
    const uint64_t elapsed =
        furi_hal_rtc_inactivity_magic == FURI_HAL_RTC_INACTIVITY_MAGIC ?
            furi_hal_rtc_last_sleep_seconds :
            0;
    FURI_CRITICAL_EXIT();
    return elapsed;
}

void furi_hal_rtc_reset_inactivity(void) {
    FURI_CRITICAL_ENTER();
    furi_hal_rtc_last_sleep_seconds = 0;
    furi_hal_rtc_inactivity_magic = FURI_HAL_RTC_INACTIVITY_MAGIC;
    FURI_CRITICAL_EXIT();
}

void furi_hal_rtc_set_timezone(bool automatic, int16_t offset_minutes) {
    if(!furi_hal_rtc_timezone_offset_is_valid(offset_minutes)) return;

    furi_hal_rtc.timezone_auto = automatic;
    furi_hal_rtc.timezone_offset_minutes = offset_minutes;
    furi_hal_rtc_apply_timezone();
    furi_hal_rtc_save_locale();
}

bool furi_hal_rtc_get_timezone_auto(void) {
    return furi_hal_rtc.timezone_auto;
}

int16_t furi_hal_rtc_get_timezone_offset_minutes(void) {
    return furi_hal_rtc.timezone_offset_minutes;
}

FuriHalRtcLocaleTimeFormat furi_hal_rtc_get_locale_timeformat(void) {
    return furi_hal_rtc.locale_timeformat;
}

void furi_hal_rtc_set_locale_timeformat(FuriHalRtcLocaleTimeFormat format) {
    if(format > FuriHalRtcLocaleTimeFormat12h) return;
    furi_hal_rtc.locale_timeformat = format;
    furi_hal_rtc_save_locale();
}

FuriHalRtcLocaleDateFormat furi_hal_rtc_get_locale_dateformat(void) {
    return furi_hal_rtc.locale_dateformat;
}

void furi_hal_rtc_set_locale_dateformat(FuriHalRtcLocaleDateFormat format) {
    if(format > FuriHalRtcLocaleDateFormatYMD) return;
    furi_hal_rtc.locale_dateformat = format;
    furi_hal_rtc_save_locale();
}

FuriHalRtcLocaleUnits furi_hal_rtc_get_locale_units(void) {
    return furi_hal_rtc.locale_units;
}

void furi_hal_rtc_set_locale_units(FuriHalRtcLocaleUnits format) {
    if(format > FuriHalRtcLocaleUnitsImperial) return;
    furi_hal_rtc.locale_units = format;
    furi_hal_rtc_save_locale();
}
