#include <furi.h>
#include <furi_hal_version.h>

#include <gui/gui.h>
#include <dolphin/dolphin.h>
#include <dolphin/helpers/dolphin_state.h>

#include <assets_icons.h>

/* Momentum's passport, art and behaviour.
 *
 * It differs from OFW's in more than looks: OFW drew two frame pieces plus nine
 * portraits (3 moods x 3 levels), Momentum draws one 128x64 frame and three
 * portraits with no level variant. Asset packs are authored against Momentum's
 * icon names, so a pack's Passport art could never bind while this app asked for
 * OFW's. */

typedef struct {
    FuriSemaphore* semaphore;
    DolphinStats* stats;
    ViewPort* view_port;
    bool progress_total;
} PassportContext;

static void input_callback(InputEvent* input, void* _ctx) {
    PassportContext* ctx = _ctx;

    if((input->type == InputTypeShort) && (input->key == InputKeyOk)) {
        ctx->progress_total = !ctx->progress_total;
        view_port_update(ctx->view_port);
    }

    if((input->type == InputTypeShort) && (input->key == InputKeyBack)) {
        furi_semaphore_release(ctx->semaphore);
    }
}

static void render_callback(Canvas* canvas, void* _ctx) {
    PassportContext* ctx = _ctx;
    DolphinStats* stats = ctx->stats;

    char level_str[12];
    char xp_str[12];
    const char* mood_str;
    const Icon* portrait;

    if(stats->butthurt <= 4) {
        portrait = &I_passport_happy_46x49;
        mood_str = "Mood: Happy";
    } else if(stats->butthurt <= 7) {
        portrait = &I_passport_okay_46x49;
        mood_str = "Mood: Bored";
    } else if(stats->butthurt <= 9) {
        portrait = &I_passport_bad_46x49;
        mood_str = "Mood: Sad";
    } else {
        portrait = &I_passport_bad_46x49;
        mood_str = "Mood: Angry";
    }

    bool max_level = (size_t)stats->level == DOLPHIN_LEVEL_COUNT + 1;

    uint32_t xp_have;
    uint32_t xp_target;
    if(ctx->progress_total) {
        xp_have = stats->icounter;
        xp_target = DOLPHIN_LEVELS[DOLPHIN_LEVEL_COUNT - 1];
    } else {
        xp_have = dolphin_state_xp_above_last_levelup(stats->icounter);
        xp_target = dolphin_state_xp_to_levelup(stats->icounter) + xp_have;
    }

    uint32_t xp_progress = 0;
    if(!max_level && xp_target > 0) {
        xp_progress = (xp_target - xp_have) * 64 / xp_target;
    }

    // multipass
    canvas_draw_icon(canvas, 0, 0, &I_passport_128x64);

    // portrait
    canvas_draw_icon(canvas, 11, 2, portrait);

    const char* my_name = furi_hal_version_get_name_ptr();
    snprintf(level_str, sizeof(level_str), "Level: %hu", stats->level);
    canvas_draw_str(canvas, 59, 10, my_name ? my_name : "Unknown");
    canvas_draw_str(canvas, 59, 22, mood_str);
    canvas_draw_str(canvas, 59, 34, level_str);

    if(max_level) {
        snprintf(xp_str, sizeof(xp_str), "Max Level!");
    } else {
        snprintf(xp_str, sizeof(xp_str), "%lu/%lu", (unsigned long)xp_have, (unsigned long)xp_target);
    }
    canvas_set_font(canvas, FontBatteryPercent);
    canvas_draw_str(canvas, 59, 42, xp_str);
    canvas_set_font(canvas, FontSecondary);

    canvas_set_color(canvas, ColorWhite);
    canvas_draw_box(canvas, 123 - xp_progress, 45, xp_progress + (xp_progress > 0), 5);
    canvas_set_color(canvas, ColorBlack);
}

int32_t passport_app(void* p) {
    UNUSED(p);
    FuriSemaphore* semaphore = furi_semaphore_alloc(1, 0);
    ViewPort* view_port = view_port_alloc();

    Dolphin* dolphin = furi_record_open(RECORD_DOLPHIN);
    DolphinStats stats = dolphin_stats(dolphin);
    furi_record_close(RECORD_DOLPHIN);

    PassportContext* ctx = malloc(sizeof(PassportContext));
    ctx->stats = &stats;
    ctx->view_port = view_port;
    ctx->semaphore = semaphore;
    ctx->progress_total = false;

    view_port_draw_callback_set(view_port, render_callback, ctx);
    view_port_input_callback_set(view_port, input_callback, ctx);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    view_port_update(view_port);

    furi_check(furi_semaphore_acquire(semaphore, FuriWaitForever) == FuriStatusOk);

    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_GUI);
    furi_semaphore_free(semaphore);
    free(ctx);

    return 0;
}
