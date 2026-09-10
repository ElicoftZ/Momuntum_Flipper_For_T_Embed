#include <furi.h>
#include <furi_hal.h>

#include <gui/gui.h>
#include <gui/elements.h>

#include "clock_app.h"

static void clock_input_callback(InputEvent* input_event, void* context) {
    furi_assert(context);
    FuriMessageQueue* event_queue = context;

    PluginEvent event = {.type = EventTypeKey, .input = *input_event};
    furi_message_queue_put(event_queue, &event, FuriWaitForever);
}

static void clock_format_elapsed(char* text, size_t text_size, uint64_t seconds) {
    const uint64_t days = seconds / 86400ULL;
    const uint64_t hours = (seconds / 3600ULL) % 24ULL;
    const uint64_t minutes = (seconds / 60ULL) % 60ULL;
    const uint64_t remainder = seconds % 60ULL;

    if(days > 0) {
        snprintf(
            text,
            text_size,
            "%llud %02llu:%02llu",
            (unsigned long long)days,
            (unsigned long long)hours,
            (unsigned long long)minutes);
    } else {
        snprintf(
            text,
            text_size,
            "%02llu:%02llu:%02llu",
            (unsigned long long)hours,
            (unsigned long long)minutes,
            (unsigned long long)remainder);
    }
}

static void clock_render_callback(Canvas* canvas, void* context) {
    ClockState* state = context;
    if(furi_mutex_acquire(state->mutex, 200) != FuriStatusOk) return;

    const bool show_stopwatch = state->show_stopwatch;
    const bool stopwatch_running = state->stopwatch_running;
    uint64_t elapsed = state->stopwatch_elapsed_seconds;
    if(show_stopwatch && stopwatch_running) {
        const uint64_t now = furi_hal_rtc_get_counter_seconds();
        if(now >= state->stopwatch_start_seconds) {
            elapsed += now - state->stopwatch_start_seconds;
        }
    } else if(!show_stopwatch) {
        elapsed = furi_hal_rtc_get_inactivity_seconds();
    }

    furi_mutex_release(state->mutex);

    char elapsed_string[24];
    clock_format_elapsed(elapsed_string, sizeof(elapsed_string), elapsed);

    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas,
        64,
        9,
        AlignCenter,
        AlignCenter,
        show_stopwatch ? "Stopwatch" : "Inactive for");

    canvas_set_font(canvas, FontBigNumbers);
    canvas_draw_str_aligned(canvas, 64, 32, AlignCenter, AlignCenter, elapsed_string);

    if(show_stopwatch) {
        elements_button_left(canvas, "Reset");
        elements_button_center(canvas, stopwatch_running ? "Stop" : "Start");
    } else {
        elements_button_center(canvas, "Stopwatch");
    }
}

static void clock_tick(void* context) {
    furi_assert(context);
    FuriMessageQueue* event_queue = context;
    PluginEvent event = {.type = EventTypeTick};
    furi_message_queue_put(event_queue, &event, 0);
}

int32_t clock_app(void* p) {
    UNUSED(p);
    ClockState* state = calloc(1, sizeof(ClockState));
    if(!state) return 255;

    state->event_queue = furi_message_queue_alloc(8, sizeof(PluginEvent));
    if(!state->event_queue) {
        free(state);
        return 255;
    }

    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    if(!state->mutex) {
        furi_message_queue_free(state->event_queue);
        free(state);
        return 255;
    }

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, clock_render_callback, state);
    view_port_input_callback_set(view_port, clock_input_callback, state->event_queue);

    FuriTimer* timer = furi_timer_alloc(clock_tick, FuriTimerTypePeriodic, state->event_queue);
    if(!timer) {
        view_port_free(view_port);
        furi_mutex_free(state->mutex);
        furi_message_queue_free(state->event_queue);
        free(state);
        return 255;
    }

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    furi_timer_start(timer, furi_kernel_get_tick_frequency());

    PluginEvent event;
    for(bool processing = true; processing;) {
        if(furi_message_queue_get(state->event_queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }

        if(furi_mutex_acquire(state->mutex, FuriWaitForever) != FuriStatusOk) continue;

        if(event.type == EventTypeKey && event.input.type == InputTypeShort) {
            const uint64_t now = furi_hal_rtc_get_counter_seconds();

            if(event.input.key == InputKeyOk) {
                if(!state->show_stopwatch) {
                    state->show_stopwatch = true;
                } else if(state->stopwatch_running) {
                    if(now >= state->stopwatch_start_seconds) {
                        state->stopwatch_elapsed_seconds +=
                            now - state->stopwatch_start_seconds;
                    }
                    state->stopwatch_running = false;
                } else {
                    state->stopwatch_start_seconds = now;
                    state->stopwatch_running = true;
                }
            } else if(event.input.key == InputKeyLeft && state->show_stopwatch) {
                state->stopwatch_running = false;
                state->stopwatch_start_seconds = 0;
                state->stopwatch_elapsed_seconds = 0;
            } else if(event.input.key == InputKeyBack) {
                if(state->show_stopwatch) {
                    state->show_stopwatch = false;
                } else {
                    processing = false;
                }
            }
        }

        furi_mutex_release(state->mutex);
        view_port_update(view_port);
    }

    furi_timer_free(timer);
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    furi_record_close(RECORD_GUI);
    view_port_free(view_port);
    furi_message_queue_free(state->event_queue);
    furi_mutex_free(state->mutex);
    free(state);

    return 0;
}
