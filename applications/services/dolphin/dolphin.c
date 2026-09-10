#include "dolphin_i.h"

#include <furi_hal.h>
#include <storage/storage.h>
#include <esp_attr.h>

#define TAG "Dolphin"

#define DOLPHIN_LOCK_EVENT_FLAG (0x1)
#define EVENT_QUEUE_SIZE        (8)

#define SECONDS_IN_TICKS(x)    ((x) * 1000UL)
#define MINUTES_IN_TICKS(x)    (SECONDS_IN_TICKS(x) * 60UL)
#define HOURS_IN_TICKS(x)      (MINUTES_IN_TICKS(x) * 60UL)

#include <momentum/momentum.h>

#define FLUSH_TIMEOUT_TICKS (SECONDS_IN_TICKS(30UL))

#define DOLPHIN_BORED_AFTER_S     (6ULL * 60ULL * 60ULL)
#define DOLPHIN_SAD_AFTER_S       (24ULL * 60ULL * 60ULL)
#define DOLPHIN_ANGRY_AFTER_S     (72ULL * 60ULL * 60ULL)
#define DOLPHIN_BUTTHURT_BORED    5
#define DOLPHIN_BUTTHURT_SAD      8
#define DOLPHIN_BUTTHURT_ANGRY    10

#ifndef DOLPHIN_DEBUG
#define CLEAR_LIMITS_PERIOD_TICKS        (HOURS_IN_TICKS(24UL))
#else
#define CLEAR_LIMITS_PERIOD_TICKS        (MINUTES_IN_TICKS(1))
#endif

/* This baseline is intentionally not a calendar timestamp. RTC slow memory
 * and the monotonic slow-clock counter survive deep sleep and warm resets, so
 * a wrong date, timezone, or failed WiFi sync can never freeze daily XP. */
#define DOLPHIN_LIMIT_WINDOW_MAGIC 0x44585031UL // "DXP1"
RTC_NOINIT_ATTR static uint32_t dolphin_limit_window_magic;
RTC_NOINIT_ATTR static uint64_t dolphin_limit_window_start_seconds;

/* butthurt_timer of 0 means "OFF": the dolphin never gets sadder on its own.
 * Starting a periodic timer with a zero period would instead fire it
 * continuously, so stop it in that case. */
static void dolphin_butthurt_timer_restart(Dolphin* dolphin) {
    const uint32_t seconds = momentum_settings.butthurt_timer;
    if(seconds > 0) {
        furi_event_loop_timer_start(dolphin->butthurt_timer, SECONDS_IN_TICKS(seconds));
    } else {
        furi_event_loop_timer_stop(dolphin->butthurt_timer);
    }
}

static void dolphin_event_send_async(Dolphin* dolphin, DolphinEvent* event);
static void dolphin_event_send_wait(Dolphin* dolphin, DolphinEvent* event);

// Public API

void dolphin_deed(DolphinDeed deed) {
    Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN);

    DolphinEvent event;
    event.type = DolphinEventTypeDeed;
    event.deed = deed;

    dolphin_event_send_async(dolphin, &event);

    furi_record_close(RECORD_DOLPHIN);
}

void dolphin_get_settings(Dolphin* dolphin, DolphinSettings* settings) {
    furi_check(dolphin);
    furi_check(settings);

    DolphinEvent event;
    event.type = DolphinEventTypeSettingsGet;
    event.settings = settings;
    dolphin_event_send_wait(dolphin, &event);
}

void dolphin_set_settings(Dolphin* dolphin, DolphinSettings* settings) {
    furi_check(dolphin);
    furi_check(settings);

    DolphinEvent event;
    event.type = DolphinEventTypeSettingsSet;
    event.settings = settings;
    dolphin_event_send_wait(dolphin, &event);
}

DolphinStats dolphin_stats(Dolphin* dolphin) {
    furi_check(dolphin);

    DolphinStats stats;
    DolphinEvent event;

    event.type = DolphinEventTypeStats;
    event.stats = &stats;

    dolphin_event_send_wait(dolphin, &event);

    return stats;
}

void dolphin_flush(Dolphin* dolphin) {
    furi_check(dolphin);

    DolphinEvent event;
    event.type = DolphinEventTypeFlush;

    dolphin_event_send_wait(dolphin, &event);
}

void dolphin_reload_state(Dolphin* dolphin) {
    furi_check(dolphin);

    DolphinEvent event;
    event.type = DolphinEventTypeReloadState;
    dolphin_event_send_wait(dolphin, &event);
}

void dolphin_prepare_for_sleep(Dolphin* dolphin) {
    furi_check(dolphin);

    DolphinEvent event;
    event.type = DolphinEventTypePrepareSleep;
    dolphin_event_send_wait(dolphin, &event);
}

void dolphin_upgrade_level(Dolphin* dolphin) {
    furi_check(dolphin);

    DolphinEvent event;
    event.type = DolphinEventTypeLevel;

    dolphin_event_send_async(dolphin, &event);
}

FuriPubSub* dolphin_get_pubsub(Dolphin* dolphin) {
    furi_check(dolphin);
    return dolphin->pubsub;
}

// Private functions

static uint8_t dolphin_inactivity_target(uint64_t elapsed, const char** mood) {
    if(elapsed >= DOLPHIN_ANGRY_AFTER_S) {
        *mood = "angry";
        return DOLPHIN_BUTTHURT_ANGRY;
    }
    if(elapsed >= DOLPHIN_SAD_AFTER_S) {
        *mood = "sad";
        return DOLPHIN_BUTTHURT_SAD;
    }
    if(elapsed >= DOLPHIN_BORED_AFTER_S) {
        *mood = "bored";
        return DOLPHIN_BUTTHURT_BORED;
    }

    *mood = "unchanged";
    return 0;
}

static void dolphin_inactivity_apply(Dolphin* dolphin) {
    const uint64_t elapsed = furi_hal_rtc_get_inactivity_seconds();
    const char* mood = NULL;
    const uint8_t target = dolphin_inactivity_target(elapsed, &mood);
    const int32_t previous_butthurt = dolphin->state->data.butthurt;

    while(dolphin->state->data.butthurt < target) {
        dolphin_state_butthurted(dolphin->state);
    }

    if(dolphin->state->data.butthurt != previous_butthurt) {
        dolphin_state_save(dolphin->state);
        DolphinPubsubEvent pubsub_event = DolphinPubsubEventUpdate;
        furi_pubsub_publish(dolphin->pubsub, &pubsub_event);
    }

    FURI_LOG_I(
        TAG,
        "Last deep sleep %llu seconds: mood %s, butthurt %ld -> %ld",
        (unsigned long long)elapsed,
        mood,
        (long)previous_butthurt,
        (long)dolphin->state->data.butthurt);
}

static void dolphin_butthurt_timer_callback(void* context) {
    Dolphin* dolphin = context;
    furi_assert(dolphin);

    FURI_LOG_I(TAG, "Increase butthurt");
    const int32_t previous_butthurt = dolphin->state->data.butthurt;
    dolphin_state_butthurted(dolphin->state);
    dolphin_state_save(dolphin->state);

    if(dolphin->state->data.butthurt != previous_butthurt) {
        DolphinPubsubEvent pubsub_event = DolphinPubsubEventUpdate;
        furi_pubsub_publish(dolphin->pubsub, &pubsub_event);
    }
}

static void dolphin_flush_timer_callback(void* context) {
    Dolphin* dolphin = context;
    furi_assert(dolphin);

    FURI_LOG_I(TAG, "Flush stats");
    dolphin_state_save(dolphin->state);
}

static void dolphin_clear_limits_timer_callback(void* context) {
    Dolphin* dolphin = context;
    furi_assert(dolphin);

    FURI_LOG_I(TAG, "Clear limits after elapsed 24-hour window");
    dolphin_state_clear_limits(dolphin->state);
    dolphin_state_save(dolphin->state);
    dolphin_limit_window_start_seconds = furi_hal_rtc_get_counter_seconds();
    dolphin_limit_window_magic = DOLPHIN_LIMIT_WINDOW_MAGIC;
    furi_event_loop_timer_start(dolphin->clear_limits_timer, CLEAR_LIMITS_PERIOD_TICKS);
}

static Dolphin* dolphin_alloc(void) {
    Dolphin* dolphin = malloc(sizeof(Dolphin));

    dolphin->state = dolphin_state_alloc();
    dolphin->pubsub = furi_pubsub_alloc();
    dolphin->event_queue = furi_message_queue_alloc(EVENT_QUEUE_SIZE, sizeof(DolphinEvent));
    dolphin->event_loop = furi_event_loop_alloc();

    dolphin->butthurt_timer = furi_event_loop_timer_alloc(
        dolphin->event_loop,
        dolphin_butthurt_timer_callback,
        FuriEventLoopTimerTypePeriodic,
        dolphin);

    dolphin->flush_timer = furi_event_loop_timer_alloc(
        dolphin->event_loop, dolphin_flush_timer_callback, FuriEventLoopTimerTypeOnce, dolphin);

    dolphin->clear_limits_timer = furi_event_loop_timer_alloc(
        dolphin->event_loop,
        dolphin_clear_limits_timer_callback,
        FuriEventLoopTimerTypeOnce,
        dolphin);

    return dolphin;
}

static void dolphin_event_send_async(Dolphin* dolphin, DolphinEvent* event) {
    furi_assert(dolphin);
    furi_assert(event);
    event->flag = NULL;
    furi_check(
        furi_message_queue_put(dolphin->event_queue, event, FuriWaitForever) == FuriStatusOk);
}

static void dolphin_event_send_wait(Dolphin* dolphin, DolphinEvent* event) {
    furi_assert(dolphin);
    furi_assert(event);

    event->flag = furi_event_flag_alloc();
    furi_check(
        furi_message_queue_put(dolphin->event_queue, event, FuriWaitForever) == FuriStatusOk);
    furi_check(
        furi_event_flag_wait(
            event->flag, DOLPHIN_LOCK_EVENT_FLAG, FuriFlagWaitAny, FuriWaitForever) ==
        DOLPHIN_LOCK_EVENT_FLAG);
    furi_event_flag_free(event->flag);
}

static void dolphin_event_release(DolphinEvent* event) {
    if(event->flag) {
        furi_event_flag_set(event->flag, DOLPHIN_LOCK_EVENT_FLAG);
    }
}

static void dolphin_schedule_daily_limits(Dolphin* dolphin) {
    const uint64_t now = furi_hal_rtc_get_counter_seconds();
    const uint64_t period_seconds = CLEAR_LIMITS_PERIOD_TICKS / 1000UL;
    const bool baseline_valid = dolphin_limit_window_magic == DOLPHIN_LIMIT_WINDOW_MAGIC &&
                                dolphin_limit_window_start_seconds <= now;
    uint64_t elapsed = baseline_valid ? now - dolphin_limit_window_start_seconds : period_seconds;

    /* The first boot of this firmware clears any calendar-stuck counters. A
     * later warm/deep-sleep boot resumes the same monotonic 24-hour window. */
    if(elapsed >= period_seconds) {
        dolphin_state_clear_limits(dolphin->state);
        dolphin_state_save(dolphin->state);
        dolphin_limit_window_start_seconds = now;
        dolphin_limit_window_magic = DOLPHIN_LIMIT_WINDOW_MAGIC;
        elapsed = 0;
    }

    const uint32_t remaining_ticks = (uint32_t)((period_seconds - elapsed) * 1000ULL);
    furi_event_loop_timer_start(dolphin->clear_limits_timer, remaining_ticks);
    FURI_LOG_I(
        TAG,
        "Daily XP limits reset in %lu seconds (elapsed RTC)",
        (unsigned long)(remaining_ticks / 1000UL));
}

static void dolphin_process_event(FuriEventLoopObject* object, void* context) {
    UNUSED(object);

    Dolphin* dolphin = context;
    DolphinEvent event;

    FuriStatus status = furi_message_queue_get(dolphin->event_queue, &event, 0);
    furi_check(status == FuriStatusOk);

    if(event.type == DolphinEventTypeDeed) {
        dolphin_state_on_deed(dolphin->state, event.deed);

        DolphinPubsubEvent pubsub_event = DolphinPubsubEventUpdate;
        furi_pubsub_publish(dolphin->pubsub, &pubsub_event);
        dolphin_butthurt_timer_restart(dolphin);
        furi_event_loop_timer_start(dolphin->flush_timer, FLUSH_TIMEOUT_TICKS);

    } else if(event.type == DolphinEventTypeStats) {
        event.stats->icounter = dolphin->state->data.icounter;
        event.stats->butthurt = (dolphin->state->data.flags & DolphinFlagHappyMode) ?
                                    0 :
                                    dolphin->state->data.butthurt;
        /* Retained in the public struct for binary compatibility only. */
        event.stats->timestamp = 0;
        event.stats->level = dolphin_get_level(dolphin->state->data.icounter);
        event.stats->level_up_is_pending =
            !dolphin_state_xp_to_levelup(dolphin->state->data.icounter);

    } else if(event.type == DolphinEventTypeFlush) {
        /* Flush means "persist now", so it must WRITE -- it used to re-arm the
         * same delay timer a deed arms, which pushed the save 30 s further out
         * instead of forcing it. That silently broke every caller: the backup
         * app calls this to capture fresh XP before sweeping NVS and got the
         * stale blob, or none at all on a device that had never idled 30 s
         * after earning XP -- so a backup recorded zero and the restore put
         * zero back. dolphin_flush() sends this synchronously, so the NVS write
         * has completed by the time it returns. dolphin_state_save() is a no-op
         * unless the state is dirty, so this costs no flash cycle when nothing
         * has changed. */
        furi_event_loop_timer_stop(dolphin->flush_timer);
        dolphin_state_save(dolphin->state);

    } else if(event.type == DolphinEventTypeLevel) {
        dolphin_state_increase_level(dolphin->state);
        furi_event_loop_timer_start(dolphin->flush_timer, FLUSH_TIMEOUT_TICKS);

    } else if(event.type == DolphinEventTypeReloadState) {
        dolphin_state_load(dolphin->state);
        dolphin_inactivity_apply(dolphin);
        dolphin_butthurt_timer_restart(dolphin);

    } else if(event.type == DolphinEventTypeSettingsGet) {
        event.settings->happy_mode = dolphin->state->data.flags & DolphinFlagHappyMode;

    } else if(event.type == DolphinEventTypeSettingsSet) {
        dolphin->state->data.flags &= ~DolphinFlagHappyMode;
        if(event.settings->happy_mode) dolphin->state->data.flags |= DolphinFlagHappyMode;
        dolphin->state->dirty = true;
        dolphin_state_save(dolphin->state);

    } else if(event.type == DolphinEventTypePrepareSleep) {
        furi_event_loop_timer_stop(dolphin->flush_timer);
        dolphin_state_save(dolphin->state);

    } else {
        furi_crash();
    }

    dolphin_event_release(&event);
}

static void dolphin_storage_callback(const void* message, void* context) {
    furi_assert(context);
    Dolphin* dolphin = context;
    const StorageEvent* event = message;

    if(event->type == StorageEventTypeCardMount) {
        DolphinEvent event = {
            .type = DolphinEventTypeReloadState,
        };

        dolphin_event_send_async(dolphin, &event);
    }
}

static void dolphin_init_state(Dolphin* dolphin) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    furi_pubsub_subscribe(storage_get_pubsub(storage), dolphin_storage_callback, dolphin);

    /* Dolphin state is on INT_PATH, so a missing SD card must not leave the
     * freshly allocated state uninitialized. */
    dolphin_state_load(dolphin->state);
}

// Application thread

int32_t dolphin_srv(void* p) {
    UNUSED(p);

    if(furi_hal_rtc_get_boot_mode() != FuriHalRtcBootModeNormal) {
        FURI_LOG_W(TAG, "Skipping start in special boot mode");

        furi_thread_suspend(furi_thread_get_current_id());
        return 0;
    }

    Dolphin* dolphin = dolphin_alloc();
    furi_record_create(RECORD_DOLPHIN, dolphin);

    dolphin_init_state(dolphin);
    dolphin_inactivity_apply(dolphin);

    furi_event_loop_subscribe_message_queue(
        dolphin->event_loop,
        dolphin->event_queue,
        FuriEventLoopEventIn,
        dolphin_process_event,
        dolphin);

    dolphin_butthurt_timer_restart(dolphin);
    dolphin_schedule_daily_limits(dolphin);

    furi_event_loop_run(dolphin->event_loop);

    return 0;
}
