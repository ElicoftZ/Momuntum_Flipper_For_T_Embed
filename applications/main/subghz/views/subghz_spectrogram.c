#include "subghz_spectrogram.h"

#include "../helpers/subghz_spectrogram_worker.h"

#include <furi.h>
#include <furi_hal_display.h>
#include <input/input.h>
#include <esp_heap_caps.h>

#include <stdio.h>
#include <string.h>

#define TAG "SubGhzSpectrogram"

#define SPECTROGRAM_HEADER_HEIGHT 34U
#define SPECTROGRAM_HEATMAP_Y     35U
#define SPECTROGRAM_FOOTER_Y      147U

typedef struct {
    const char* name;
    uint32_t start;
    uint32_t stop;
    bool narrow_bandwidth;
} SubGhzSpectrogramBand;

static const SubGhzSpectrogramBand subghz_spectrogram_bands[] = {
    {.name = "315 WIDE", .start = 300000000U, .stop = 348000000U, .narrow_bandwidth = false},
    {.name = "390 WIDE", .start = 378000000U, .stop = 418000000U, .narrow_bandwidth = false},
    {.name = "433 WIDE", .start = 418000000U, .stop = 464000000U, .narrow_bandwidth = false},
    {.name = "433 ISM", .start = 433050000U, .stop = 434790000U, .narrow_bandwidth = true},
    {.name = "UHF WIDE", .start = 779000000U, .stop = 928000000U, .narrow_bandwidth = false},
    {.name = "868 ISM", .start = 863000000U, .stop = 870000000U, .narrow_bandwidth = true},
    {.name = "915 ISM", .start = 902000000U, .stop = 928000000U, .narrow_bandwidth = false},
};

struct SubGhzSpectrogram {
    View* view;
    Gui* gui;
    Canvas* direct_canvas;
    FuriPubSub* input_events;
    FuriPubSubSubscription* input_subscription;
    FuriMutex* render_mutex;
    SubGhzSpectrogramWorker* worker;
    SubGhzSpectrogramCallback callback;
    void* callback_context;
    uint16_t* framebuffer;
    uint16_t width;
    uint16_t height;
    uint8_t band_index;
    volatile bool active;
    bool paused;
    bool allocation_failed;
    uint32_t peak_frequency;
    int8_t peak_rssi;
    uint32_t sweep_count;
};

static uint16_t subghz_spectrogram_rgb(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t native =
        ((uint16_t)(r & 0xF8U) << 8) | ((uint16_t)(g & 0xFCU) << 3) | (b >> 3);
    return (uint16_t)((native << 8) | (native >> 8));
}

static uint8_t subghz_spectrogram_lerp(uint8_t a, uint8_t b, uint8_t amount) {
    return (uint8_t)(a + (((int16_t)b - a) * amount) / 255);
}

static uint16_t subghz_spectrogram_palette(int16_t rssi) {
    typedef struct {
        uint8_t position;
        uint8_t r;
        uint8_t g;
        uint8_t b;
    } ColorStop;

    static const ColorStop stops[] = {
        {0, 2, 4, 14},
        {42, 30, 12, 105},
        {86, 15, 45, 230},
        {126, 0, 214, 255},
        {164, 25, 235, 116},
        {198, 255, 235, 20},
        {226, 255, 70, 10},
        {255, 255, 245, 255},
    };

    int16_t normalized = ((rssi + 115) * 255) / 80;
    if(normalized < 0) normalized = 0;
    if(normalized > 255) normalized = 255;

    for(size_t i = 1; i < COUNT_OF(stops); i++) {
        if(normalized <= stops[i].position) {
            uint8_t span = stops[i].position - stops[i - 1].position;
            uint8_t amount = span ?
                                 (uint8_t)(((normalized - stops[i - 1].position) * 255) / span) :
                                 0;
            return subghz_spectrogram_rgb(
                subghz_spectrogram_lerp(stops[i - 1].r, stops[i].r, amount),
                subghz_spectrogram_lerp(stops[i - 1].g, stops[i].g, amount),
                subghz_spectrogram_lerp(stops[i - 1].b, stops[i].b, amount));
        }
    }

    return subghz_spectrogram_rgb(255, 245, 255);
}

static void subghz_spectrogram_fill_rect(
    SubGhzSpectrogram* instance,
    uint16_t x,
    uint16_t y,
    uint16_t width,
    uint16_t height,
    uint16_t color) {
    if(x >= instance->width || y >= instance->height) return;
    if(x + width > instance->width) width = instance->width - x;
    if(y + height > instance->height) height = instance->height - y;
    for(uint16_t row = 0; row < height; row++) {
        uint16_t* destination =
            &instance->framebuffer[(size_t)(y + row) * instance->width + x];
        for(uint16_t column = 0; column < width; column++) destination[column] = color;
    }
}

static const uint8_t* subghz_spectrogram_glyph(char character) {
    static const uint8_t digits[10][5] = {
        {0x3E, 0x51, 0x49, 0x45, 0x3E},
        {0x00, 0x42, 0x7F, 0x40, 0x00},
        {0x42, 0x61, 0x51, 0x49, 0x46},
        {0x21, 0x41, 0x45, 0x4B, 0x31},
        {0x18, 0x14, 0x12, 0x7F, 0x10},
        {0x27, 0x45, 0x45, 0x45, 0x39},
        {0x3C, 0x4A, 0x49, 0x49, 0x30},
        {0x01, 0x71, 0x09, 0x05, 0x03},
        {0x36, 0x49, 0x49, 0x49, 0x36},
        {0x06, 0x49, 0x49, 0x29, 0x1E},
    };
    static const uint8_t letters[26][5] = {
        {0x7E, 0x11, 0x11, 0x11, 0x7E}, {0x7F, 0x49, 0x49, 0x49, 0x36},
        {0x3E, 0x41, 0x41, 0x41, 0x22}, {0x7F, 0x41, 0x41, 0x22, 0x1C},
        {0x7F, 0x49, 0x49, 0x49, 0x41}, {0x7F, 0x09, 0x09, 0x09, 0x01},
        {0x3E, 0x41, 0x49, 0x49, 0x7A}, {0x7F, 0x08, 0x08, 0x08, 0x7F},
        {0x00, 0x41, 0x7F, 0x41, 0x00}, {0x20, 0x40, 0x41, 0x3F, 0x01},
        {0x7F, 0x08, 0x14, 0x22, 0x41}, {0x7F, 0x40, 0x40, 0x40, 0x40},
        {0x7F, 0x02, 0x0C, 0x02, 0x7F}, {0x7F, 0x04, 0x08, 0x10, 0x7F},
        {0x3E, 0x41, 0x41, 0x41, 0x3E}, {0x7F, 0x09, 0x09, 0x09, 0x06},
        {0x3E, 0x41, 0x51, 0x21, 0x5E}, {0x7F, 0x09, 0x19, 0x29, 0x46},
        {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7F, 0x01, 0x01},
        {0x3F, 0x40, 0x40, 0x40, 0x3F}, {0x1F, 0x20, 0x40, 0x20, 0x1F},
        {0x3F, 0x40, 0x38, 0x40, 0x3F}, {0x63, 0x14, 0x08, 0x14, 0x63},
        {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43},
    };
    static const uint8_t blank[5] = {0};
    static const uint8_t dash[5] = {0x08, 0x08, 0x08, 0x08, 0x08};
    static const uint8_t dot[5] = {0x00, 0x60, 0x60, 0x00, 0x00};
    static const uint8_t colon[5] = {0x00, 0x36, 0x36, 0x00, 0x00};
    static const uint8_t slash[5] = {0x20, 0x10, 0x08, 0x04, 0x02};

    if(character >= '0' && character <= '9') return digits[character - '0'];
    if(character >= 'A' && character <= 'Z') return letters[character - 'A'];
    if(character == '-') return dash;
    if(character == '.') return dot;
    if(character == ':') return colon;
    if(character == '/') return slash;
    return blank;
}

static void subghz_spectrogram_draw_text(
    SubGhzSpectrogram* instance,
    uint16_t x,
    uint16_t y,
    const char* text,
    uint8_t scale,
    uint16_t color) {
    while(*text && x < instance->width) {
        const uint8_t* glyph = subghz_spectrogram_glyph(*text++);
        for(uint8_t column = 0; column < 5; column++) {
            for(uint8_t row = 0; row < 7; row++) {
                if(glyph[column] & (1U << row)) {
                    subghz_spectrogram_fill_rect(
                        instance,
                        x + column * scale,
                        y + row * scale,
                        scale,
                        scale,
                        color);
                }
            }
        }
        x += 6U * scale;
    }
}

static uint16_t subghz_spectrogram_text_width(const char* text, uint8_t scale) {
    size_t length = strlen(text);
    return length ? (uint16_t)((length * 6U - 1U) * scale) : 0;
}

static void subghz_spectrogram_format_frequency(
    char* output,
    size_t output_size,
    uint32_t frequency) {
    snprintf(
        output,
        output_size,
        "%lu.%03lu",
        (unsigned long)(frequency / 1000000U),
        (unsigned long)((frequency % 1000000U) / 1000U));
}

static void subghz_spectrogram_draw_chrome(SubGhzSpectrogram* instance) {
    const uint16_t dark = subghz_spectrogram_rgb(3, 7, 19);
    const uint16_t header = subghz_spectrogram_rgb(6, 20, 39);
    const uint16_t footer = subghz_spectrogram_rgb(5, 14, 29);
    const uint16_t text = subghz_spectrogram_rgb(224, 240, 255);
    const uint16_t cyan = subghz_spectrogram_rgb(26, 229, 255);
    const uint16_t purple = subghz_spectrogram_rgb(182, 83, 255);
    const uint16_t yellow = subghz_spectrogram_rgb(255, 218, 45);

    subghz_spectrogram_fill_rect(
        instance, 0, 0, instance->width, SPECTROGRAM_HEADER_HEIGHT, header);
    subghz_spectrogram_fill_rect(instance, 0, 34, instance->width, 1, cyan);
    subghz_spectrogram_fill_rect(instance, 0, 146, instance->width, 1, purple);
    subghz_spectrogram_fill_rect(
        instance, 0, SPECTROGRAM_FOOTER_Y, instance->width,
        instance->height - SPECTROGRAM_FOOTER_Y, footer);

    subghz_spectrogram_draw_text(instance, 8, 4, "RF SPECTROGRAM", 2, text);

    const SubGhzSpectrogramBand* band = &subghz_spectrogram_bands[instance->band_index];
    uint16_t pill_width = subghz_spectrogram_text_width(band->name, 1) + 12;
    uint16_t pill_x = instance->width - pill_width - 7;
    subghz_spectrogram_fill_rect(instance, pill_x, 7, pill_width, 11, purple);
    subghz_spectrogram_draw_text(instance, pill_x + 6, 9, band->name, 1, dark);

    char peak_text[64];
    if(instance->sweep_count) {
        snprintf(
            peak_text,
            sizeof(peak_text),
            "PEAK %lu.%03lu MHZ  %d DBM",
            (unsigned long)(instance->peak_frequency / 1000000U),
            (unsigned long)((instance->peak_frequency % 1000000U) / 1000U),
            (int)instance->peak_rssi);
    } else {
        snprintf(peak_text, sizeof(peak_text), "WAITING FOR FIRST SWEEP");
    }
    subghz_spectrogram_draw_text(instance, 8, 23, peak_text, 1, text);

    const char* state = instance->paused ? "PAUSED" : "RUN";
    uint16_t state_width = subghz_spectrogram_text_width(state, 1);
    subghz_spectrogram_draw_text(
        instance,
        instance->width - state_width - 8,
        23,
        state,
        1,
        instance->paused ? yellow : cyan);

    char start_text[16];
    char stop_text[16];
    subghz_spectrogram_format_frequency(start_text, sizeof(start_text), band->start);
    subghz_spectrogram_format_frequency(stop_text, sizeof(stop_text), band->stop);
    subghz_spectrogram_draw_text(instance, 4, 150, start_text, 1, text);
    uint16_t stop_width = subghz_spectrogram_text_width(stop_text, 1);
    subghz_spectrogram_draw_text(
        instance, instance->width - stop_width - 4, 150, stop_text, 1, text);

    uint16_t legend_x = (instance->width - 80U) / 2U;
    for(uint16_t x = 0; x < 80; x++) {
        int16_t rssi = -115 + (int16_t)((80U * x) / 79U);
        subghz_spectrogram_fill_rect(
            instance, legend_x + x, 151, 1, 5, subghz_spectrogram_palette(rssi));
    }

    const char* controls = instance->paused ?
                               "ROTATE BAND   OK RESUME   BACK EXIT" :
                               "ROTATE BAND   OK PAUSE    BACK EXIT";
    uint16_t controls_width = subghz_spectrogram_text_width(controls, 1);
    subghz_spectrogram_draw_text(
        instance,
        (instance->width - controls_width) / 2U,
        161,
        controls,
        1,
        text);
}

static void subghz_spectrogram_present(SubGhzSpectrogram* instance) {
    if(!instance->active || !instance->framebuffer) return;
    subghz_spectrogram_draw_chrome(instance);
    furi_hal_display_blit_rgb565(
        0, 0, instance->width, instance->height, instance->framebuffer);
}

static void subghz_spectrogram_clear_heatmap(SubGhzSpectrogram* instance) {
    subghz_spectrogram_fill_rect(
        instance,
        0,
        SPECTROGRAM_HEATMAP_Y,
        instance->width,
        SPECTROGRAM_FOOTER_Y - SPECTROGRAM_HEATMAP_Y - 1U,
        subghz_spectrogram_palette(-127));
    instance->peak_frequency = 0;
    instance->peak_rssi = -127;
    instance->sweep_count = 0;
}

static void subghz_spectrogram_worker_callback(
    void* context,
    const int8_t* rssi_bins,
    size_t bin_count,
    uint32_t range_start,
    uint32_t range_stop,
    uint32_t peak_frequency,
    int8_t peak_rssi) {
    SubGhzSpectrogram* instance = context;
    if(!instance->active || !instance->framebuffer) return;

    if(furi_mutex_acquire(instance->render_mutex, 100) != FuriStatusOk) return;
    if(!instance->active) {
        furi_mutex_release(instance->render_mutex);
        return;
    }
    const SubGhzSpectrogramBand* band = &subghz_spectrogram_bands[instance->band_index];
    if(range_start != band->start || range_stop != band->stop) {
        furi_mutex_release(instance->render_mutex);
        return;
    }

    const uint16_t heatmap_height =
        SPECTROGRAM_FOOTER_Y - SPECTROGRAM_HEATMAP_Y - 1U;
    uint16_t* first_row =
        &instance->framebuffer[(size_t)SPECTROGRAM_HEATMAP_Y * instance->width];
    memmove(
        first_row + instance->width,
        first_row,
        (size_t)(heatmap_height - 1U) * instance->width * sizeof(uint16_t));

    for(uint16_t x = 0; x < instance->width; x++) {
        size_t bin = ((size_t)x * bin_count) / instance->width;
        if(bin >= bin_count) bin = bin_count - 1U;
        first_row[x] = subghz_spectrogram_palette(rssi_bins[bin]);
    }

    instance->peak_frequency = peak_frequency;
    instance->peak_rssi = peak_rssi;
    instance->sweep_count++;
    subghz_spectrogram_present(instance);
    furi_mutex_release(instance->render_mutex);
}

static void subghz_spectrogram_select_band(SubGhzSpectrogram* instance, int8_t direction) {
    size_t band_count = COUNT_OF(subghz_spectrogram_bands);
    if(direction < 0) {
        instance->band_index =
            instance->band_index ? instance->band_index - 1U : band_count - 1U;
    } else {
        instance->band_index = (instance->band_index + 1U) % band_count;
    }

    const SubGhzSpectrogramBand* band = &subghz_spectrogram_bands[instance->band_index];
    subghz_spectrogram_worker_set_range(
        instance->worker, band->start, band->stop, band->narrow_bandwidth);
    subghz_spectrogram_clear_heatmap(instance);
}

static void subghz_spectrogram_input_callback(const void* value, void* context) {
    SubGhzSpectrogram* instance = context;
    const InputEvent* event = value;
    if(!instance->active || event->type != InputTypeShort) return;

    if(event->key == InputKeyBack) {
        if(instance->callback) {
            instance->callback(SubGhzCustomEventViewSpectrogramBack, instance->callback_context);
        }
        return;
    }

    if(event->key == InputKeyOk) {
        if(furi_mutex_acquire(instance->render_mutex, 100) != FuriStatusOk) return;
        instance->paused = !instance->paused;
        subghz_spectrogram_worker_set_paused(instance->worker, instance->paused);
        subghz_spectrogram_present(instance);
        furi_mutex_release(instance->render_mutex);
        return;
    }

    if(event->key == InputKeyUp || event->key == InputKeyDown ||
       event->key == InputKeyLeft || event->key == InputKeyRight) {
        int8_t direction =
            (event->key == InputKeyUp || event->key == InputKeyLeft) ? -1 : 1;
        if(furi_mutex_acquire(instance->render_mutex, 100) != FuriStatusOk) return;
        subghz_spectrogram_select_band(instance, direction);
        subghz_spectrogram_present(instance);
        furi_mutex_release(instance->render_mutex);
    }
}

static void subghz_spectrogram_draw(Canvas* canvas, void* model) {
    UNUSED(model);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 18, 28, "RF Spectrogram");
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str(canvas, 22, 43, "Opening color display...");
}

static bool subghz_spectrogram_view_input(InputEvent* event, void* context) {
    UNUSED(context);
    return event->key != InputKeyBack;
}

static void subghz_spectrogram_enter(void* context) {
    SubGhzSpectrogram* instance = context;
    instance->width = furi_hal_display_get_h_res();
    instance->height = furi_hal_display_get_v_res();
    instance->allocation_failed = false;
    instance->paused = false;

    size_t framebuffer_size =
        (size_t)instance->width * instance->height * sizeof(uint16_t);
    instance->framebuffer =
        heap_caps_malloc(framebuffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!instance->framebuffer) {
        instance->framebuffer = heap_caps_malloc(framebuffer_size, MALLOC_CAP_8BIT);
    }
    if(!instance->framebuffer) {
        FURI_LOG_E(TAG, "framebuffer allocation failed (%u bytes)", (unsigned)framebuffer_size);
        instance->allocation_failed = true;
        view_commit_model(instance->view, true);
        return;
    }

    instance->active = true;
    subghz_spectrogram_fill_rect(
        instance,
        0,
        0,
        instance->width,
        instance->height,
        subghz_spectrogram_rgb(2, 4, 14));
    subghz_spectrogram_clear_heatmap(instance);

    instance->input_events = furi_record_open(RECORD_INPUT_EVENTS);
    instance->input_subscription = furi_pubsub_subscribe(
        instance->input_events, subghz_spectrogram_input_callback, instance);
    instance->direct_canvas = gui_direct_draw_acquire(instance->gui);

    instance->worker = subghz_spectrogram_worker_alloc();
    subghz_spectrogram_worker_set_callback(
        instance->worker, subghz_spectrogram_worker_callback, instance);
    const SubGhzSpectrogramBand* band = &subghz_spectrogram_bands[instance->band_index];
    subghz_spectrogram_worker_set_range(
        instance->worker, band->start, band->stop, band->narrow_bandwidth);

    if(furi_mutex_acquire(instance->render_mutex, FuriWaitForever) == FuriStatusOk) {
        subghz_spectrogram_present(instance);
        furi_mutex_release(instance->render_mutex);
    }
    subghz_spectrogram_worker_start(instance->worker);
}

static void subghz_spectrogram_exit(void* context) {
    SubGhzSpectrogram* instance = context;
    if(instance->allocation_failed) {
        instance->allocation_failed = false;
        return;
    }

    instance->active = false;
    if(instance->input_subscription) {
        furi_pubsub_unsubscribe(instance->input_events, instance->input_subscription);
        instance->input_subscription = NULL;
    }
    if(instance->input_events) {
        furi_record_close(RECORD_INPUT_EVENTS);
        instance->input_events = NULL;
    }
    if(instance->worker) {
        subghz_spectrogram_worker_stop(instance->worker);
        subghz_spectrogram_worker_free(instance->worker);
        instance->worker = NULL;
    }
    if(instance->direct_canvas) {
        gui_direct_draw_release(instance->gui);
        instance->direct_canvas = NULL;
    }
    if(instance->framebuffer) {
        free(instance->framebuffer);
        instance->framebuffer = NULL;
    }
}

SubGhzSpectrogram* subghz_spectrogram_alloc(Gui* gui) {
    furi_assert(gui);
    SubGhzSpectrogram* instance = calloc(1, sizeof(SubGhzSpectrogram));
    instance->gui = gui;
    instance->render_mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    instance->band_index = 3; /* Start on the common 433.92 MHz ISM band. */

    instance->view = view_alloc();
    view_set_context(instance->view, instance);
    view_set_draw_callback(instance->view, subghz_spectrogram_draw);
    view_set_input_callback(instance->view, subghz_spectrogram_view_input);
    view_set_enter_callback(instance->view, subghz_spectrogram_enter);
    view_set_exit_callback(instance->view, subghz_spectrogram_exit);
    return instance;
}

void subghz_spectrogram_free(SubGhzSpectrogram* instance) {
    furi_assert(instance);
    view_free(instance->view);
    furi_mutex_free(instance->render_mutex);
    free(instance);
}

View* subghz_spectrogram_get_view(SubGhzSpectrogram* instance) {
    furi_assert(instance);
    return instance->view;
}

void subghz_spectrogram_set_callback(
    SubGhzSpectrogram* instance,
    SubGhzSpectrogramCallback callback,
    void* context) {
    furi_assert(instance);
    instance->callback = callback;
    instance->callback_context = context;
}
