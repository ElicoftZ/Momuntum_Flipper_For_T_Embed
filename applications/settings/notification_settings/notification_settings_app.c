/**
 * @file notification_settings_app.c
 * @brief ESP32 notification settings — adapted from STM32 unleashed-firmware.
 *
 * STM32-only features removed: LCD contrast, LCD inversion, LED brightness,
 * RGB backlight. Everything else kept as close to the original as possible.
 */
#include <furi.h>
#include <furi_hal_rtc.h>
#include <furi_hal_light.h>
#include <notification/notification_app.h>
#include <notification/notification_messages.h>
#include <gui/modules/variable_item_list.h>
#include <gui/view_dispatcher.h>
#include <lib/toolbox/value_index.h>
#include <stdio.h>
#include "notification_settings_color_picker.h"

/* View IDs in this app's dispatcher. */
#define NOTIFICATION_SETTINGS_VIEW_LIST   0
#define NOTIFICATION_SETTINGS_VIEW_PICKER 1

/* Custom event: "UI Background → Custom" was selected, open the picker. */
#define NOTIFICATION_SETTINGS_EVENT_OPEN_PICKER 1
/* Rebuilding the list frees the VariableItem whose callback is running, so
 * the Colors row defers it through the dispatcher instead of calling it
 * inline -- doing it inline dropped the selection (and touched freed item). */
#define NOTIFICATION_SETTINGS_EVENT_REBUILD_LIST 2

typedef struct {
    NotificationApp* notification;
    ViewDispatcher* view_dispatcher;
    VariableItemList* variable_item_list;
    NotificationColorPicker* color_picker;
    /* The "UI Background" item — kept so we can grow its value cycle with the
     * "Custom" trigger and the resulting "#RRGGBB" entry. */
    VariableItem* ui_bg_item;
    /* Direct refs so the night_shift lock callback doesn't depend on a
     * hardcoded index — the menu shape changes per board (LED items gated
     * on BOARD_HAS_RGB_LED) and per future feature additions. */
    VariableItem* night_shift_start_item;
    VariableItem* night_shift_end_item;
    /* Which color the picker is editing: 0 = UI Background, or
     * 1..FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS for gradient stop n-1. */
    uint8_t picker_target;
    /* List row index of "Color 1". The stop rows share one callback and
     * variable_item has no per-item user data, so the stop is derived from
     * the selected row. Storing VariableItem* here does NOT work: the items
     * live in an M-LIB array of values, so every later add can realloc and
     * leave the saved pointers dangling. */
    uint8_t ui_bg_first_stop_row;
    /* Saved before the picker opens, so cancelling puts the stop back. */
    uint8_t picker_prev_index;
    uint32_t picker_prev_custom;
    /* The list row that opened the picker. Restored on the way back: the
     * OK that confirms Save otherwise lands on the list behind the picker
     * and moves the highlight off the colour being edited. */
    uint8_t picker_prev_selected;
} NotificationAppSettings;

static const NotificationSequence sequence_note_c = {
    &message_note_c5,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

#define BACKLIGHT_COUNT 17
const char* const backlight_text[BACKLIGHT_COUNT] = {
    "20%", "25%", "30%", "35%", "40%", "45%", "50%",
    "55%", "60%", "65%", "70%", "75%", "80%", "85%", "90%", "95%", "100%",
};
const float backlight_value[BACKLIGHT_COUNT] = {
    0.20f, 0.25f, 0.30f, 0.35f, 0.40f, 0.45f, 0.50f,
    0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f,
};

#define VOLUME_COUNT 21
const char* const volume_text[VOLUME_COUNT] = {
    "0%",  "5%",  "10%", "15%", "20%", "25%", "30%", "35%", "40%", "45%",  "50%",
    "55%", "60%", "65%", "70%", "75%", "80%", "85%", "90%", "95%", "100%",
};
const float volume_value[VOLUME_COUNT] = {
    0.00f, 0.05f, 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f, 0.45f, 0.50f,
    0.55f, 0.60f, 0.65f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f,
};

#define DELAY_COUNT 12
const char* const delay_text[DELAY_COUNT] = {
    "Always ON",
    "2s",
    "5s",
    "10s",
    "15s",
    "30s",
    "60s",
    "90s",
    "120s",
    "5min",
    "10min",
    "30min",
};
const uint32_t delay_value[DELAY_COUNT] =
    {0, 2000, 5000, 10000, 15000, 30000, 60000, 90000, 120000, 300000, 600000, 1800000};

/* Deep sleep. Separate table from delay_value: the useful range is much
 * narrower, and "Always ON" would read as never sleeping rather than never
 * dimming. */
#define SLEEP_COUNT 8
const char* const sleep_text[SLEEP_COUNT] = {
    "Never", "15s", "30s", "60s", "2min", "3min", "4min", "5min",
};
const uint32_t sleep_value[SLEEP_COUNT] =
    {0, 15000, 30000, 60000, 120000, 180000, 240000, 300000};


// --- NIGHT SHIFT ---
#define NIGHT_SHIFT_COUNT 7
const char* const night_shift_text[NIGHT_SHIFT_COUNT] =
    {"OFF", "-10%", "-20%", "-30%", "-40%", "-50%", "-60%"};
const float night_shift_value[NIGHT_SHIFT_COUNT] = {
    1.0f, 0.9f, 0.8f, 0.7f, 0.6f, 0.5f, 0.4f,
};

#define NIGHT_SHIFT_START_COUNT 14
const char* const night_shift_start_text[NIGHT_SHIFT_START_COUNT] = {
    "17:00", "17:30", "18:00", "18:30", "19:00", "19:30", "20:00",
    "20:30", "21:00", "21:30", "22:00", "22:30", "23:00", "23:30",
};
const uint32_t night_shift_start_value[NIGHT_SHIFT_START_COUNT] = {
    1020, 1050, 1080, 1110, 1140, 1170, 1200,
    1230, 1260, 1290, 1320, 1350, 1380, 1410,
};

#define NIGHT_SHIFT_END_COUNT 14
const char* const night_shift_end_text[NIGHT_SHIFT_END_COUNT] = {
    "05:00", "05:30", "06:00", "06:30", "07:00", "07:30", "08:00",
    "08:30", "09:00", "09:30", "10:00", "10:30", "11:00", "11:30",
};
const uint32_t night_shift_end_value[NIGHT_SHIFT_END_COUNT] = {
    300, 330, 360, 390, 410, 440, 470,
    500, 530, 560, 590, 620, 650, 680,
};
// --- NIGHT SHIFT END ---

static void backlight_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, backlight_text[index]);
    app->notification->settings.display_brightness = backlight_value[index];

    notification_message(app->notification, &sequence_display_backlight_force_on);
}

static void screen_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, delay_text[index]);
    app->notification->settings.display_off_delay_ms = delay_value[index];

    if((delay_value[index] == 0) & (furi_timer_is_running(app->notification->display_timer))) {
        furi_timer_stop(app->notification->display_timer);
    }
    notification_message(app->notification, &sequence_display_backlight_on);
}

static void volume_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, volume_text[index]);
    app->notification->settings.speaker_volume = volume_value[index];
    notification_message(app->notification, &sequence_note_c);
}

// --- NIGHT SHIFT ---

static void sleep_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, sleep_text[index]);
    app->notification->settings.display_sleep_delay_ms = sleep_value[index];

    /* Re-arm against the new delay straight away. */
    notification_message(app->notification, &sequence_display_backlight_on);
}

static void night_shift_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, night_shift_text[index]);
    app->notification->settings.night_shift = night_shift_value[index];

    /* Lock/unlock Start + End via the stored pointers — index-free, so the
     * lock works regardless of how the menu is shuffled by board-gating
     * (LED items only appear when BOARD_HAS_RGB_LED). */
    if(app->night_shift_start_item) {
        variable_item_set_locked(app->night_shift_start_item, index == 0, "Night Shift\nOFF!");
    }
    if(app->night_shift_end_item) {
        variable_item_set_locked(app->night_shift_end_item, index == 0, "Night Shift\nOFF!");
    }

    /* Demo the night shift brightness briefly */
    app->notification->current_night_shift = night_shift_value[index];
    notification_message(app->notification, &sequence_display_backlight_force_on);

    if(night_shift_value[index] != 1) {
        night_shift_timer_start(app->notification);
        if(furi_timer_is_running(app->notification->night_shift_demo_timer)) {
            furi_timer_stop(app->notification->night_shift_demo_timer);
        }
        furi_timer_start(app->notification->night_shift_demo_timer, furi_ms_to_ticks(1200));
    } else {
        night_shift_timer_stop(app->notification);
        if(furi_timer_is_running(app->notification->night_shift_demo_timer))
            furi_timer_stop(app->notification->night_shift_demo_timer);
    }

    notification_message_save_settings(app->notification);
}

static void night_shift_start_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, night_shift_start_text[index]);
    app->notification->settings.night_shift_start = night_shift_start_value[index];

    notification_message_save_settings(app->notification);
}

static void night_shift_end_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);

    variable_item_set_current_value_text(item, night_shift_end_text[index]);
    app->notification->settings.night_shift_end = night_shift_end_value[index];

    notification_message_save_settings(app->notification);
}

// --- NIGHT SHIFT END ---

// --- LED COLOR (T-Embed Plus WS2812 ring) ---
#define LED_COLOR_COUNT 10
const char* const led_color_text[LED_COLOR_COUNT] = {
    "Off",
    "Red",
    "Green",
    "Blue",
    "Cyan",
    "Magenta",
    "Yellow",
    "White",
    "Orange",
    "Purple",
};
/* Each value is packed 0x00RRGGBB at full saturation per active channel.
 * The actual hardware output = (channel * settings.led_brightness), so the
 * brightness slider is the single dial controlling current draw and
 * perceived intensity. Purple uses 0x80 R + 0xFF B for a perceptually
 * correct violet that doesn't look like plain blue. */
const uint32_t led_color_value[LED_COLOR_COUNT] = {
    0x00000000, /* Off */
    0x00FF0000, /* Red */
    0x0000FF00, /* Green */
    0x000000FF, /* Blue */
    0x0000FFFF, /* Cyan */
    0x00FF1DCE, /* Magenta — #FF1DCE */
    0x00FFFF00, /* Yellow — #FFFF00 */
    0x00FFFFFF, /* White */
    0x00FFA500, /* Orange — #FFA500 */
    0x008F00FF, /* Purple — #8F00FF */
};

static void led_color_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, led_color_text[index]);
    app->notification->settings.led_color = led_color_value[index];
    notification_apply_led_color(app->notification);     /* live preview */
    notification_message_save_settings(app->notification); /* persist */
}

/* LED brightness has its own table with much finer low-end resolution because
 * WS2812 LEDs are very bright at low duty cycles. The notification.c apply
 * function cubes this value (cubic gamma correction) to match perceptual
 * brightness on the T-Embed Plus, so user-set 1%–10% all clamp to off,
 * 20% drives ~0.8%, 30% drives ~2.7%, 50% drives 12.5%, 100% stays 100%. */
#define LED_BRIGHTNESS_COUNT 20
const char* const led_brightness_text[LED_BRIGHTNESS_COUNT] = {
    "1%", "2%", "5%", "10%", "15%", "20%", "25%", "30%", "35%", "40%",
    "45%", "50%", "60%", "70%", "75%", "80%", "85%", "90%", "95%", "100%",
};
const float led_brightness_value[LED_BRIGHTNESS_COUNT] = {
    0.01f, 0.02f, 0.05f, 0.10f, 0.15f, 0.20f, 0.25f, 0.30f, 0.35f, 0.40f,
    0.45f, 0.50f, 0.60f, 0.70f, 0.75f, 0.80f, 0.85f, 0.90f, 0.95f, 1.00f,
};

static void led_brightness_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, led_brightness_text[index]);
    app->notification->settings.led_brightness = led_brightness_value[index];
    notification_apply_led_color(app->notification);     /* live preview */
    notification_message_save_settings(app->notification); /* persist */
}

static void led_timeout_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    /* Reuse the same DELAY_COUNT table as the backlight timeout. */
    variable_item_set_current_value_text(item, delay_text[index]);
    app->notification->settings.led_off_delay_ms = delay_value[index];
    notification_apply_led_color(app->notification);     /* re-arms timer with new value */
    notification_message_save_settings(app->notification); /* persist */
}

#define LED_EFFECT_COUNT 9
const char* const led_effect_text[LED_EFFECT_COUNT] = {
    "Solid",     /* 0 */
    "Off",       /* 1 */
    "Breathing", /* 2 */
    "Chase",     /* 3 */
    "Rainbow",   /* 4 */
    "Strobe",    /* 5 */
    "Cylon",     /* 6 */
    "Sparkle",   /* 7 */
    "Wipe",      /* 8 */
};

static void led_effect_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, led_effect_text[index]);
    app->notification->settings.led_effect = index;
    notification_apply_led_color(app->notification);     /* live preview */
    notification_message_save_settings(app->notification); /* persist */
}

#define LED_SPEED_COUNT 5
const char* const led_speed_text[LED_SPEED_COUNT] = {
    "Slowest", "Slow", "Normal", "Fast", "Fastest",
};
const uint32_t led_speed_value[LED_SPEED_COUNT] = {
    100, 60, 33, 16, 8, /* milliseconds per animation tick */
};

static void led_speed_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, led_speed_text[index]);
    app->notification->settings.led_speed_tick_ms = led_speed_value[index];
    notification_apply_led_color(app->notification);     /* restart effect at new speed */
    notification_message_save_settings(app->notification); /* persist */
}
// --- LED COLOR END ---

// --- UI COLOR (LCD bichromatic palette) ---
/* The UI color presets — order matches ui_color_value[] in notification.c.
 * Last entry "Spectrum" is a sentinel; selecting it kicks off the periodic
 * hue-cycle timer in the notification service. Used by both
 * "UI Background" (fg_color = field around UI elements, default Orange) and
 * "UI Foreground" (bg_color = drawn UI elements, default Black). */
#define UI_COLOR_COUNT 11
const char* const ui_color_text[UI_COLOR_COUNT] = {
    "Black", "Orange", "Red", "Green", "Blue", "Cyan",
    "Magenta", "Yellow", "White", "Purple", "Spectrum",
};

/* The UI Background value cycle is dynamic:
 *   [0 .. UI_COLOR_COUNT-1]   fixed presets (Black .. Spectrum)
 *   [UI_COLOR_COUNT]          "#RRGGBB"  — only once a custom color is defined
 *   [last]                    "Custom"   — opens the color picker
 * Rebuild the item's value count + currently-shown value from persisted state. */
static void ui_bg_item_sync(NotificationAppSettings* app) {
    NotificationSettings* s = &app->notification->settings;
    uint8_t count = s->ui_custom_color_set ? (UI_COLOR_COUNT + 2) : (UI_COLOR_COUNT + 1);
    variable_item_set_values_count(app->ui_bg_item, count);

    if(s->ui_color_index == UI_COLOR_CUSTOM_INDEX && s->ui_custom_color_set) {
        char buf[10];
        snprintf(buf, sizeof(buf), "#%06X", (unsigned)(s->ui_custom_color & 0xFFFFFF));
        variable_item_set_current_value_index(app->ui_bg_item, UI_COLOR_COUNT); /* hex slot */
        variable_item_set_current_value_text(app->ui_bg_item, buf);
    } else {
        uint8_t idx = s->ui_color_index < UI_COLOR_COUNT ? s->ui_color_index : 1;
        variable_item_set_current_value_index(app->ui_bg_item, idx);
        variable_item_set_current_value_text(app->ui_bg_item, ui_color_text[idx]);
    }
}

static void ui_bg_color_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    NotificationSettings* s = &app->notification->settings;
    uint8_t index = variable_item_get_current_value_index(item);

    if(index < UI_COLOR_COUNT) {
        /* A fixed preset (Black .. Spectrum). */
        variable_item_set_current_value_text(item, ui_color_text[index]);
        s->ui_color_index = index;
        notification_apply_ui_color(app->notification);     /* live preview */
        notification_message_save_settings(app->notification); /* persist */
        return;
    }

    if(s->ui_custom_color_set && index == UI_COLOR_COUNT) {
        /* The persisted "#RRGGBB" custom color entry. */
        char buf[10];
        snprintf(buf, sizeof(buf), "#%06X", (unsigned)(s->ui_custom_color & 0xFFFFFF));
        variable_item_set_current_value_text(item, buf);
        s->ui_color_index = UI_COLOR_CUSTOM_INDEX;
        notification_apply_ui_color(app->notification);
        notification_message_save_settings(app->notification);
        return;
    }

    /* Trailing "Custom" entry → open the picker. Deferred via a custom event so
     * we don't switch views from inside the variable_item_list model lock. */
    variable_item_set_current_value_text(item, "Custom");
    view_dispatcher_send_custom_event(
        app->view_dispatcher, NOTIFICATION_SETTINGS_EVENT_OPEN_PICKER);
}

/* Picker result: Save stores + selects the new custom color; Back reverts the
 * live preview and restores the previous selection. Either way return to the list. */
static void rebuild_settings_list(NotificationAppSettings* app);
static void ui_bg_stop_item_sync(NotificationSettings* s, VariableItem* item, uint8_t stop);

/* Live preview while picking a gradient stop. The picker's built-in preview
 * writes fg_color, which the display ignores whenever a gradient is on -- so
 * the colour appeared not to change at all. Write the in-progress value into
 * the stop and rebuild the ramp instead, which previews the real thing. */
static void ui_bg_stop_preview(void* context, uint32_t rgb) {
    NotificationAppSettings* app = context;
    if(app->picker_target == 0) return;
    const uint8_t stop = app->picker_target - 1;
    if(stop >= FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS) return;

    NotificationSettings* s = &app->notification->settings;
    s->ui_bg_stop_custom[stop] = rgb & 0xFFFFFF;
    s->ui_bg_stop_index[stop] = UI_COLOR_CUSTOM_INDEX;
    notification_apply_ui_gradient(app->notification); /* not persisted yet */
}

static void ui_bg_color_picker_result(void* context, uint32_t rgb, bool confirmed) {
    NotificationAppSettings* app = context;
    NotificationSettings* s = &app->notification->settings;

    /* A gradient stop was being edited rather than the flat UI Background. */
    if(app->picker_target > 0) {
        const uint8_t stop = app->picker_target - 1;
        app->picker_target = 0;
        if(stop < FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS) {
            if(confirmed) {
                s->ui_bg_stop_custom[stop] = rgb & 0xFFFFFF;
                s->ui_bg_stop_index[stop] = UI_COLOR_CUSTOM_INDEX;
                notification_message_save_settings(app->notification);
            } else {
                /* Undo whatever the live preview wrote. */
                s->ui_bg_stop_index[stop] = app->picker_prev_index;
                s->ui_bg_stop_custom[stop] = app->picker_prev_custom;
            }
        }
        notification_apply_ui_color(app->notification);
        /* The row still reads "Custom"; rebuilding re-syncs every row's text
         * from settings without needing a pointer to this one. */
        view_dispatcher_send_custom_event(
            app->view_dispatcher, NOTIFICATION_SETTINGS_EVENT_REBUILD_LIST);
        view_dispatcher_switch_to_view(app->view_dispatcher, NOTIFICATION_SETTINGS_VIEW_LIST);
        variable_item_list_set_selected_item(
            app->variable_item_list, app->picker_prev_selected);
        return;
    }

    if(confirmed) {
        s->ui_custom_color = rgb & 0xFFFFFF;
        s->ui_custom_color_set = true;
        s->ui_color_index = UI_COLOR_CUSTOM_INDEX;
        ui_bg_item_sync(app); /* grow the cycle + select the new "#RRGGBB" entry */
        notification_apply_ui_color(app->notification);
        notification_message_save_settings(app->notification);
    } else {
        /* Cancelled — restore selection text and revert the previewed color. */
        ui_bg_item_sync(app);
        notification_apply_ui_color(app->notification);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, NOTIFICATION_SETTINGS_VIEW_LIST);
    variable_item_list_set_selected_item(app->variable_item_list, app->picker_prev_selected);
}

static bool notification_settings_custom_event(void* context, uint32_t event) {
    NotificationAppSettings* app = context;
    if(event == NOTIFICATION_SETTINGS_EVENT_REBUILD_LIST) {
        rebuild_settings_list(app);
        return true;
    }
    if(event == NOTIFICATION_SETTINGS_EVENT_OPEN_PICKER) {
        NotificationSettings* s = &app->notification->settings;
        /* Stop the Spectrum hue-cycle timer (if the last selection was Spectrum)
         * so it doesn't overwrite the picker's live preview every tick. */
        if(app->notification->ui_spectrum_timer &&
           furi_timer_is_running(app->notification->ui_spectrum_timer)) {
            furi_timer_stop(app->notification->ui_spectrum_timer);
        }
        uint32_t init;
        if(app->picker_target > 0) {
            const uint8_t stop = app->picker_target - 1;
            /* Start from the stop's own color so re-editing is not a reset. */
            init = s->ui_bg_stop_custom[stop] & 0xFFFFFF;
            app->picker_prev_index = s->ui_bg_stop_index[stop];
            app->picker_prev_custom = s->ui_bg_stop_custom[stop];
            notification_color_picker_set_preview_callback(
                app->color_picker, ui_bg_stop_preview, app);
        } else {
            notification_color_picker_set_preview_callback(app->color_picker, NULL, NULL);
            init = s->ui_custom_color_set ? (s->ui_custom_color & 0xFFFFFF) : UI_CUSTOM_DEFAULT_RGB;
        }
        app->picker_prev_selected =
            variable_item_list_get_selected_item_index(app->variable_item_list);
        notification_color_picker_set_color(app->color_picker, init);
        view_dispatcher_switch_to_view(app->view_dispatcher, NOTIFICATION_SETTINGS_VIEW_PICKER);
        return true;
    }
    return false;
}

static void ui_fg_color_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, ui_color_text[index]);
    app->notification->settings.ui_fg_color_index = index;
    notification_apply_ui_color(app->notification);     /* live preview */
    notification_message_save_settings(app->notification); /* persist */
}
// --- UI COLOR END ---

static uint32_t notification_app_settings_exit(void* context) {
    UNUSED(context);
    return VIEW_NONE;
}

/* --- UI Background gradient rows --- */
static const char* const ui_bg_count_text[FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS] = {
    "1", "2", "3", "4",
};
static const char* const ui_bg_stop_label[FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS] = {
    "Color 1", "Color 2", "Color 3", "Color 4",
};
static const char* const ui_bg_mix_text[FuriHalDisplayGradientModeCount] = {
    "Linear", "Smooth", "Bands", "Mirror",
};
static const char* const ui_bg_dir_text[2] = {"Vertical", "Horizontal"};

static void build_settings_list(NotificationAppSettings* app);

/* Rebuild the list in place, keeping the cursor where the user left it so
 * changing the stop count doesn't throw them back to the top. */
static void rebuild_settings_list(NotificationAppSettings* app) {
    const uint8_t selected =
        variable_item_list_get_selected_item_index(app->variable_item_list);
    variable_item_list_reset(app->variable_item_list);
    build_settings_list(app);
    variable_item_list_set_selected_item(app->variable_item_list, selected);
}

static NotificationAppSettings* alloc_settings(void) {
    NotificationAppSettings* app = malloc(sizeof(NotificationAppSettings));
    app->night_shift_start_item = NULL;
    app->night_shift_end_item = NULL;
    app->color_picker = NULL;
    app->ui_bg_item = NULL;
    app->picker_target = 0;
    app->picker_prev_selected = 0;
    app->notification = furi_record_open(RECORD_NOTIFICATION);

    app->variable_item_list = variable_item_list_alloc();
    View* view = variable_item_list_get_view(app->variable_item_list);
    view_set_previous_callback(view, notification_app_settings_exit);

    build_settings_list(app);

    app->color_picker = notification_color_picker_alloc();
    notification_color_picker_set_callback(app->color_picker, ui_bg_color_picker_result, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    app->view_dispatcher = view_dispatcher_alloc();
    view_dispatcher_attach_to_gui(app->view_dispatcher, gui, ViewDispatcherTypeFullscreen);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, notification_settings_custom_event);
    view_dispatcher_add_view(app->view_dispatcher, NOTIFICATION_SETTINGS_VIEW_LIST, view);
    view_dispatcher_add_view(
        app->view_dispatcher,
        NOTIFICATION_SETTINGS_VIEW_PICKER,
        notification_color_picker_get_view(app->color_picker));
    view_dispatcher_switch_to_view(app->view_dispatcher, NOTIFICATION_SETTINGS_VIEW_LIST);
    return app;
}

/* Show a stop's current color: a palette name, or #RRGGBB when custom. */
static void ui_bg_stop_item_sync(NotificationSettings* s, VariableItem* item, uint8_t stop) {
    const uint8_t idx = s->ui_bg_stop_index[stop];
    if(idx >= UI_COLOR_COUNT) {
        /* Custom: the value cycle's last slot, labelled with the hex value. */
        static char buf[FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS][10];
        snprintf(
            buf[stop], sizeof(buf[stop]), "#%06X",
            (unsigned)(s->ui_bg_stop_custom[stop] & 0xFFFFFF));
        variable_item_set_current_value_index(item, UI_COLOR_COUNT);
        variable_item_set_current_value_text(item, buf[stop]);
    } else {
        variable_item_set_current_value_index(item, idx);
        variable_item_set_current_value_text(item, ui_color_text[idx]);
    }
}

static void ui_bg_count_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    const uint8_t count = variable_item_get_current_value_index(item) + 1;
    variable_item_set_current_value_text(item, ui_bg_count_text[count - 1]);
    app->notification->settings.ui_bg_color_count = count;
    notification_apply_ui_color(app->notification);
    notification_message_save_settings(app->notification);
    /* Rows appear/disappear with the count, so the list must be rebuilt --
     * but not from inside this callback, which is running on an item the
     * rebuild would free. Defer it to the dispatcher. */
    view_dispatcher_send_custom_event(
        app->view_dispatcher, NOTIFICATION_SETTINGS_EVENT_REBUILD_LIST);
}

static void ui_bg_dir_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    const uint8_t idx = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, ui_bg_dir_text[idx]);
    app->notification->settings.ui_bg_horizontal = (idx != 0);
    notification_apply_ui_color(app->notification);
    notification_message_save_settings(app->notification);
}

static void ui_bg_mix_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    const uint8_t mix = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, ui_bg_mix_text[mix]);
    app->notification->settings.ui_bg_mix = mix;
    notification_apply_ui_color(app->notification);
    notification_message_save_settings(app->notification);
}

static void ui_bg_stop_changed(VariableItem* item) {
    NotificationAppSettings* app = variable_item_get_context(item);
    NotificationSettings* s = &app->notification->settings;
    const uint8_t index = variable_item_get_current_value_index(item);

    /* The row being edited is the selected one. */
    const uint8_t row = variable_item_list_get_selected_item_index(app->variable_item_list);
    if(row < app->ui_bg_first_stop_row) return;
    const uint8_t stop = row - app->ui_bg_first_stop_row;
    if(stop >= FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS) return;

    if(index < UI_COLOR_COUNT) {
        s->ui_bg_stop_index[stop] = index;
        variable_item_set_current_value_text(item, ui_color_text[index]);
        notification_apply_ui_color(app->notification);
        notification_message_save_settings(app->notification);
        return;
    }

    /* Last slot opens the same RGB picker the UI Background row uses. */
    app->picker_target = stop + 1;
    variable_item_set_current_value_text(item, "Custom");
    view_dispatcher_send_custom_event(
        app->view_dispatcher, NOTIFICATION_SETTINGS_EVENT_OPEN_PICKER);
}

/* Builds every row of the settings list. Split out of alloc_settings() so
 * the Colors setting can rebuild the list when the stop count changes --
 * variable_item_list has no way to hide an individual row. */
static void build_settings_list(NotificationAppSettings* app) {
    VariableItem* item;
    uint8_t value_index;

    app->ui_bg_first_stop_row = 0;

    /* LCD Backlight brightness */
    item = variable_item_list_add(
        app->variable_item_list, "LCD Backlight", BACKLIGHT_COUNT, backlight_changed, app);
    value_index = value_index_float(
        app->notification->settings.display_brightness, backlight_value, BACKLIGHT_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, backlight_text[value_index]);

    /* UI Background tint — fills the field around drawn UI elements
     * (default Orange). Presets end with "Spectrum" (HSV cycle), then a
     * "Custom" entry that opens the color picker, plus a persisted "#RRGGBB"
     * entry once a custom color has been chosen. ui_bg_item_sync() sets the
     * actual value count + current selection from saved state. */
    item = variable_item_list_add(
        app->variable_item_list, "UI Background", UI_COLOR_COUNT + 2, ui_bg_color_changed, app);
    app->ui_bg_item = item;
    ui_bg_item_sync(app);

    /* UI Foreground tint — fills the drawn UI elements themselves: text,
     * icons, borders (default Black). When both UI colors are Spectrum, the
     * foreground hue is offset 180° from the background for legibility. */
    item = variable_item_list_add(
        app->variable_item_list, "UI Foreground", UI_COLOR_COUNT, ui_fg_color_changed, app);
    value_index = app->notification->settings.ui_fg_color_index < UI_COLOR_COUNT
                      ? app->notification->settings.ui_fg_color_index : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, ui_color_text[value_index]);

    /* WS2812 ring controls — only added when the board actually has a ring
     * (furi_hal_light_pixel_count() > 0). Boards without a ring (Waveshare
     * C6 1.9, C6 1.47) get a clean menu with no inert sliders. */
    if(furi_hal_light_pixel_count() > 0) {
    /* RGB LED ring color (T-Embed Plus WS2812). Pick the preset that matches
     * the saved settings.led_color, falling back to "Off" if no match. */
    item = variable_item_list_add(
        app->variable_item_list, "LED Color", LED_COLOR_COUNT, led_color_changed, app);
    value_index = 0;
    for(uint8_t i = 0; i < LED_COLOR_COUNT; i++) {
        if(app->notification->settings.led_color == led_color_value[i]) {
            value_index = i;
            break;
        }
    }
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, led_color_text[value_index]);

    /* LED ring brightness — uses a fine-grained 1-100% table; the apply
     * function applies gamma correction so user-perceived brightness scales
     * naturally with the slider. */
    item = variable_item_list_add(
        app->variable_item_list, "LED Brightness", LED_BRIGHTNESS_COUNT, led_brightness_changed, app);
    value_index = value_index_float(
        app->notification->settings.led_brightness, led_brightness_value, LED_BRIGHTNESS_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, led_brightness_text[value_index]);

    /* LED idle-off timeout (Always ON / 2s / 5s / ... / 30min, same scale as backlight) */
    item = variable_item_list_add(
        app->variable_item_list, "LED Timeout", DELAY_COUNT, led_timeout_changed, app);
    value_index = value_index_uint32(
        app->notification->settings.led_off_delay_ms, delay_value, DELAY_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, delay_text[value_index]);

    /* LED animation effect */
    item = variable_item_list_add(
        app->variable_item_list, "LED Effect", LED_EFFECT_COUNT, led_effect_changed, app);
    value_index = app->notification->settings.led_effect < LED_EFFECT_COUNT
                      ? app->notification->settings.led_effect : 0;
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, led_effect_text[value_index]);

    /* LED animation speed (ms per tick) */
    item = variable_item_list_add(
        app->variable_item_list, "LED Speed", LED_SPEED_COUNT, led_speed_changed, app);
    value_index = value_index_uint32(
        app->notification->settings.led_speed_tick_ms, led_speed_value, LED_SPEED_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, led_speed_text[value_index]);
    } /* end if(pixel_count > 0) */

    /* Backlight timeout */
    item = variable_item_list_add(
        app->variable_item_list, "Backlight Time", DELAY_COUNT, screen_changed, app);
    value_index = value_index_uint32(
        app->notification->settings.display_off_delay_ms, delay_value, DELAY_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, delay_text[value_index]);

    /* Deep sleep timeout */
    item = variable_item_list_add(
        app->variable_item_list, "Sleep Time", SLEEP_COUNT, sleep_changed, app);
    value_index = value_index_uint32(
        app->notification->settings.display_sleep_delay_ms, sleep_value, SLEEP_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, sleep_text[value_index]);

    /* --- NIGHT SHIFT --- */
    item = variable_item_list_add(
        app->variable_item_list, "Night Shift", NIGHT_SHIFT_COUNT, night_shift_changed, app);
    value_index = value_index_float(
        app->notification->settings.night_shift, night_shift_value, NIGHT_SHIFT_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, night_shift_text[value_index]);

    item = variable_item_list_add(
        app->variable_item_list,
        " . Start",
        NIGHT_SHIFT_START_COUNT,
        night_shift_start_changed,
        app);
    value_index = value_index_uint32(
        app->notification->settings.night_shift_start,
        night_shift_start_value,
        NIGHT_SHIFT_START_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, night_shift_start_text[value_index]);
    variable_item_set_locked(
        item, (app->notification->settings.night_shift == 1), "Night Shift \nOFF!");
    app->night_shift_start_item = item;

    item = variable_item_list_add(
        app->variable_item_list, " . End", NIGHT_SHIFT_END_COUNT, night_shift_end_changed, app);
    value_index = value_index_uint32(
        app->notification->settings.night_shift_end, night_shift_end_value, NIGHT_SHIFT_END_COUNT);
    variable_item_set_current_value_index(item, value_index);
    variable_item_set_current_value_text(item, night_shift_end_text[value_index]);
    variable_item_set_locked(
        item, (app->notification->settings.night_shift == 1), "Night Shift \nOFF!");
    app->night_shift_end_item = item;
    /* --- NIGHT SHIFT END --- */

    /* Volume (respects stealth mode) */
    if(furi_hal_rtc_is_flag_set(FuriHalRtcFlagStealthMode)) {
        item = variable_item_list_add(app->variable_item_list, "Volume", 1, NULL, app);
        variable_item_set_current_value_index(item, 0);
        variable_item_set_current_value_text(item, "Stealth");
    } else {
        item = variable_item_list_add(
            app->variable_item_list, "Volume", VOLUME_COUNT, volume_changed, app);
        value_index = value_index_float(
            app->notification->settings.speaker_volume, volume_value, VOLUME_COUNT);
        variable_item_set_current_value_index(item, value_index);
        variable_item_set_current_value_text(item, volume_text[value_index]);
    }

    /* --- UI Background gradient --- */
    NotificationSettings* gs = &app->notification->settings;

    uint8_t stops = gs->ui_bg_color_count;
    if(stops < 1) stops = 1;
    if(stops > FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS) {
        stops = FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS;
    }

    item = variable_item_list_add(
        app->variable_item_list, "Colors",
        FURI_HAL_DISPLAY_GRADIENT_MAX_STOPS, ui_bg_count_changed, app);
    variable_item_set_current_value_index(item, stops - 1);
    variable_item_set_current_value_text(item, ui_bg_count_text[stops - 1]);

    /* One row per active stop. Only shown when blending -- a single color is
     * just the existing UI Background row. */
    if(stops >= 2) {
        /* variable_item_list_add appends, so the next row index is the
         * current item count. */
        app->ui_bg_first_stop_row =
            variable_item_list_get_item_count(app->variable_item_list);
        for(uint8_t i = 0; i < stops; i++) {
            item = variable_item_list_add(
                app->variable_item_list, ui_bg_stop_label[i],
                UI_COLOR_COUNT + 1, ui_bg_stop_changed, app);
            ui_bg_stop_item_sync(gs, item, i);
        }

        item = variable_item_list_add(
            app->variable_item_list, "Direction", 2, ui_bg_dir_changed, app);
        variable_item_set_current_value_index(item, gs->ui_bg_horizontal ? 1 : 0);
        variable_item_set_current_value_text(
            item, ui_bg_dir_text[gs->ui_bg_horizontal ? 1 : 0]);

        item = variable_item_list_add(
            app->variable_item_list, "Mix",
            FuriHalDisplayGradientModeCount, ui_bg_mix_changed, app);
        uint8_t mix = gs->ui_bg_mix;
        if(mix >= FuriHalDisplayGradientModeCount) mix = 0;
        variable_item_set_current_value_index(item, mix);
        variable_item_set_current_value_text(item, ui_bg_mix_text[mix]);
    }
}



static void free_settings(NotificationAppSettings* app) {
    view_dispatcher_remove_view(app->view_dispatcher, NOTIFICATION_SETTINGS_VIEW_LIST);
    view_dispatcher_remove_view(app->view_dispatcher, NOTIFICATION_SETTINGS_VIEW_PICKER);
    variable_item_list_free(app->variable_item_list);
    notification_color_picker_free(app->color_picker);
    view_dispatcher_free(app->view_dispatcher);

    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    free(app);
}

int32_t notification_settings_app(void* p) {
    UNUSED(p);
    NotificationAppSettings* app = alloc_settings();
    view_dispatcher_run(app->view_dispatcher);
    notification_message_save_settings(app->notification);
    free_settings(app);
    return 0;
}
