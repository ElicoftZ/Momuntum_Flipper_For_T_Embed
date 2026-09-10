/**
 * @file power_profiler_app.c
 * @brief Live power draw trace from the BQ27220 fuel gauge.
 *
 * The stock battery screens show instantaneous values, which is useless for the
 * question that actually matters: "what is this feature costing me?" A radio
 * that spikes to 300 mA for 200 ms and a screen that sits at 90 mA forever look
 * identical in a single reading. So this samples current continuously and draws
 * the trace, with the running average and the accumulated mAh alongside.
 *
 * Only possible because the T-Embed carries a real coulomb-counting fuel gauge;
 * furi_hal_power_get_battery_current() already reads it, so there is no driver
 * work here -- this is presentation.
 */

#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_power.h>

#include <gui/gui.h>
#include <gui/elements.h>

#define TAG "PowerProfiler"

/* One sample per pixel column of the graph. */
#define PP_GRAPH_X      2
#define PP_GRAPH_Y      22
#define PP_GRAPH_W      124
#define PP_GRAPH_H      34
#define PP_SAMPLE_COUNT PP_GRAPH_W

/* Fast enough to catch a radio burst, slow enough not to melt the I2C bus the
 * NFC reader shares. */
#define PP_SAMPLE_PERIOD_MS 200

typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* queue;
    ViewPort* view_port;
    FuriTimer* timer;

    /* Ring buffer of mA readings, negative = discharging. */
    float samples[PP_SAMPLE_COUNT];
    size_t count; /* how many are valid, saturates at PP_SAMPLE_COUNT */
    size_t head;  /* next write position */

    float now_ma;
    float min_ma;
    float max_ma;
    double sum_ma; /* for the running mean; double so long runs stay honest */
    size_t total_samples;

    /* Integrated consumption since the app opened. */
    double mah_used;

    bool charging;
    bool paused;
    /* 0 = live trace, 1 = battery info. The trace answers "how much is
     * this costing", the info page answers "out of what" -- both are
     * needed to read a draw figure as anything meaningful. */
    uint8_t page;
} PowerProfiler;

/* ------------------------------------------------------------------ */
/* Sampling                                                            */
/* ------------------------------------------------------------------ */

static void power_profiler_sample(void* context) {
    PowerProfiler* app = context;

    /* Read outside the lock: this touches I2C and can block. */
    const float ma = furi_hal_power_get_battery_current(FuriHalPowerICFuelGauge) * 1000.0f;
    const bool charging = furi_hal_power_is_charging();

    furi_mutex_acquire(app->mutex, FuriWaitForever);
    if(!app->paused) {
        app->now_ma = ma;
        app->charging = charging;

        app->samples[app->head] = ma;
        app->head = (app->head + 1) % PP_SAMPLE_COUNT;
        if(app->count < PP_SAMPLE_COUNT) app->count++;

        if(app->total_samples == 0 || ma < app->min_ma) app->min_ma = ma;
        if(app->total_samples == 0 || ma > app->max_ma) app->max_ma = ma;
        app->sum_ma += ma;
        app->total_samples++;

        /* mA over one sample period, expressed in mAh. */
        app->mah_used += ((double)(ma < 0 ? -ma : ma) * PP_SAMPLE_PERIOD_MS) / 3600000.0;
    }
    furi_mutex_release(app->mutex);

    view_port_update(app->view_port);
}

/* ------------------------------------------------------------------ */
/* Rendering                                                           */
/* ------------------------------------------------------------------ */

static void power_profiler_draw_graph(Canvas* canvas, PowerProfiler* app) {
    elements_frame(canvas, PP_GRAPH_X, PP_GRAPH_Y, PP_GRAPH_W, PP_GRAPH_H);
    if(app->count < 2) return;

    /* Autoscale to the window: absolute values, since discharge reads negative
     * and the interesting thing is magnitude. */
    float lo = 0.0f, hi = 0.0f;
    for(size_t i = 0; i < app->count; i++) {
        const float v = app->samples[i] < 0 ? -app->samples[i] : app->samples[i];
        if(i == 0 || v < lo) lo = v;
        if(i == 0 || v > hi) hi = v;
    }
    if(hi - lo < 1.0f) hi = lo + 1.0f; /* avoid a divide-by-zero flat line */

    const uint8_t inner_h = PP_GRAPH_H - 2;
    const uint8_t base_y = PP_GRAPH_Y + PP_GRAPH_H - 1;

    /* Oldest sample first: walk back from head. */
    for(size_t i = 0; i < app->count; i++) {
        const size_t idx = (app->head + PP_SAMPLE_COUNT - app->count + i) % PP_SAMPLE_COUNT;
        const float v = app->samples[idx] < 0 ? -app->samples[idx] : app->samples[idx];

        uint8_t h = (uint8_t)(((v - lo) / (hi - lo)) * (float)(inner_h - 1));
        if(h > inner_h - 1) h = inner_h - 1;

        const uint8_t x = PP_GRAPH_X + 1 + (uint8_t)i;
        if(x >= PP_GRAPH_X + PP_GRAPH_W - 1) break;
        canvas_draw_line(canvas, x, base_y - h, x, base_y - 1);
    }
}

static void power_profiler_draw_info(Canvas* canvas, PowerProfiler* app) {
    char line[48];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, "Battery Info");

    canvas_set_font(canvas, FontSecondary);

    const float volts = furi_hal_power_get_battery_voltage(FuriHalPowerICFuelGauge);
    const float temp = furi_hal_power_get_battery_temperature(FuriHalPowerICFuelGauge);
    const uint32_t remaining = furi_hal_power_get_battery_remaining_capacity();
    const uint32_t full = furi_hal_power_get_battery_full_capacity();
    const uint32_t design = furi_hal_power_get_battery_design_capacity();

    snprintf(line, sizeof(line), "%d%%  %.2f V  %d C", furi_hal_power_get_pct(),
             (double)volts, (int)temp);
    canvas_draw_str(canvas, 2, 21, line);

    snprintf(line, sizeof(line), "%lu / %lu mAh", (unsigned long)remaining,
             (unsigned long)full);
    canvas_draw_str(canvas, 2, 32, line);

    snprintf(line, sizeof(line), "Health %d%%  design %lu",
             furi_hal_power_get_bat_health_pct(), (unsigned long)design);
    canvas_draw_str(canvas, 2, 43, line);

    /* The whole point of pairing these pages: turn mA into time left. */
    const float draw = app->now_ma < 0 ? -app->now_ma : app->now_ma;
    if(app->charging) {
        canvas_draw_str(canvas, 2, 54, "Charging");
    } else if(draw > 1.0f && remaining > 0) {
        const float hours = (float)remaining / draw;
        snprintf(line, sizeof(line), "~%dh %dm left at %d mA", (int)hours,
                 (int)((hours - (int)hours) * 60.0f), (int)draw);
        canvas_draw_str(canvas, 2, 54, line);
    } else {
        canvas_draw_str(canvas, 2, 54, "Measuring...");
    }

    /* Sleep life. The draw figure is measured across a real sleep rather
     * than assumed, so this is only shown once there is one -- an invented
     * number here would be worse than none. */
    const uint32_t sleep_ua = furi_hal_power_get_sleep_current_ua();
    if(sleep_ua > 0 && remaining > 0) {
        const double hours = ((double)remaining * 1000.0) / (double)sleep_ua;
        if(hours >= 48.0) {
            snprintf(line, sizeof(line), "Sleep: %dd at %luuA", (int)(hours / 24.0),
                     (unsigned long)sleep_ua);
        } else {
            snprintf(line, sizeof(line), "Sleep: %dh at %luuA", (int)hours,
                     (unsigned long)sleep_ua);
        }
    } else {
        snprintf(line, sizeof(line), "Sleep: needs 1h+ asleep");
    }
    canvas_draw_str(canvas, 2, 63, line);
}

static void power_profiler_draw_callback(Canvas* canvas, void* ctx) {
    PowerProfiler* app = ctx;
    if(furi_mutex_acquire(app->mutex, 100) != FuriStatusOk) return;

    if(app->page == 1) {
        power_profiler_draw_info(canvas, app);
        furi_mutex_release(app->mutex);
        return;
    }

    char line[40];

    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 9, app->charging ? "Power (charging)" : "Power draw");

    canvas_set_font(canvas, FontSecondary);
    /* Show magnitude; the sign only says which way it is flowing. */
    const float now = app->now_ma < 0 ? -app->now_ma : app->now_ma;
    snprintf(line, sizeof(line), "%d mA", (int)now);
    canvas_draw_str(canvas, 2, 19, line);

    if(app->total_samples > 0) {
        const double avg = app->sum_ma / (double)app->total_samples;
        const double avg_abs = avg < 0 ? -avg : avg;
        const float peak = app->max_ma < 0 ? -app->max_ma : app->max_ma;
        const float trough = app->min_ma < 0 ? -app->min_ma : app->min_ma;
        snprintf(
            line,
            sizeof(line),
            "avg %d  pk %d",
            (int)avg_abs,
            (int)(peak > trough ? peak : trough));
        canvas_draw_str(canvas, 52, 19, line);
    }

    power_profiler_draw_graph(canvas, app);

    /* Accumulated cost, and what that means for the pack. */
    const uint32_t full = furi_hal_power_get_battery_full_capacity();
    if(full > 0) {
        snprintf(
            line,
            sizeof(line),
            "%.2f mAh  %d%% pack",
            app->mah_used,
            (int)((app->mah_used * 100.0) / (double)full));
    } else {
        snprintf(line, sizeof(line), "%.2f mAh used", app->mah_used);
    }
    canvas_draw_str(canvas, 2, 63, line);

    if(app->paused) {
        canvas_draw_str(canvas, 96, 63, "PAUSED");
    }

    furi_mutex_release(app->mutex);
}

static void power_profiler_input_callback(InputEvent* input_event, void* ctx) {
    PowerProfiler* app = ctx;
    furi_message_queue_put(app->queue, input_event, FuriWaitForever);
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

int32_t power_profiler_app(void* p) {
    UNUSED(p);

    PowerProfiler* app = malloc(sizeof(PowerProfiler));
    memset(app, 0, sizeof(PowerProfiler));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    app->queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, power_profiler_draw_callback, app);
    view_port_input_callback_set(app->view_port, power_profiler_input_callback, app);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, app->view_port, GuiLayerFullscreen);

    /* Profiling a device that sleeps mid-measurement measures nothing. */
    furi_hal_power_insomnia_enter();

    app->timer = furi_timer_alloc(power_profiler_sample, FuriTimerTypePeriodic, app);
    furi_timer_start(app->timer, furi_ms_to_ticks(PP_SAMPLE_PERIOD_MS));

    InputEvent event;
    bool running = true;
    while(running) {
        if(furi_message_queue_get(app->queue, &event, FuriWaitForever) != FuriStatusOk) {
            continue;
        }
        if(event.type != InputTypeShort && event.type != InputTypeLong) continue;

        furi_mutex_acquire(app->mutex, FuriWaitForever);
        if(event.key == InputKeyBack) {
            running = false;
        } else if(event.key == InputKeyOk && event.type == InputTypeLong) {
            /* Reset the trace: the point is measuring one thing at a time. */
            app->count = 0;
            app->head = 0;
            app->total_samples = 0;
            app->sum_ma = 0.0;
            app->mah_used = 0.0;
        } else if(event.key == InputKeyOk) {
            app->paused = !app->paused;
        } else if(event.key == InputKeyUp || event.key == InputKeyDown) {
            app->page ^= 1;
        }
        furi_mutex_release(app->mutex);

        view_port_update(app->view_port);
    }

    furi_timer_stop(app->timer);
    furi_timer_free(app->timer);
    furi_hal_power_insomnia_exit();

    gui_remove_view_port(gui, app->view_port);
    view_port_free(app->view_port);
    furi_record_close(RECORD_GUI);

    furi_message_queue_free(app->queue);
    furi_mutex_free(app->mutex);
    free(app);

    return 0;
}
