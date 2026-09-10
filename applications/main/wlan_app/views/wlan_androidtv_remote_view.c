#include "wlan_androidtv_remote_view.h"
#include "../wlan_androidtv.h"

#include <furi.h>
#include <gui/canvas.h>
#include <gui/elements.h>
#include <input/input.h>
#include <string.h>

// Button grid on the 128x64 display (header y0..11, body y13..63).
typedef struct {
    int8_t x, y, w, h;
    const char* label;
    int keycode;
} AtvButton;

static const AtvButton BUTTONS[] = {
    // Top row
    {2, 14, 38, 11, "PWR", WlanAtvKeyPower},
    {45, 14, 38, 11, "HOME", WlanAtvKeyHome},
    {88, 14, 38, 11, "BACK", WlanAtvKeyBack},
    // Left column (media transport)
    {2, 27, 22, 11, "<<", WlanAtvKeyMediaPrevious},
    {2, 40, 22, 11, ">", WlanAtvKeyMediaPlayPause},
    {2, 53, 22, 11, ">>", WlanAtvKeyMediaNext},
    // Centre D-pad
    {52, 27, 24, 11, "^", WlanAtvKeyDpadUp},
    {27, 40, 23, 11, "<", WlanAtvKeyDpadLeft},
    {52, 40, 24, 11, "OK", WlanAtvKeyDpadCenter},
    {78, 40, 22, 11, ">", WlanAtvKeyDpadRight},
    {52, 53, 24, 11, "v", WlanAtvKeyDpadDown},
    // Right column (volume)
    {104, 27, 22, 11, "V+", WlanAtvKeyVolumeUp},
    {104, 40, 22, 11, "MUT", WlanAtvKeyMute},
    {104, 53, 22, 11, "V-", WlanAtvKeyVolumeDown},
};
#define ATV_BTN_COUNT ((int)(sizeof(BUTTONS) / sizeof(BUTTONS[0])))
#define ATV_BTN_OK 8 // index of the centre OK button (initial selection)
#define ATV_FLASH_MS 160

typedef struct {
    char title[WLAN_ATV_NAME_MAX];
    bool connected;
    uint8_t selected;
    int8_t flash_index;
    uint32_t flash_tick;
    WlanAtvKeyCallback cb;
    void* cb_ctx;
} WlanAtvRemoteModel;

static void atv_remote_draw(Canvas* canvas, void* _model) {
    WlanAtvRemoteModel* m = _model;
    canvas_clear(canvas);

    // Header: device label + connection dot.
    canvas_set_font(canvas, FontSecondary);
    const char* title = m->title[0] ? m->title : "Android TV";
    FuriString* s = furi_string_alloc_set(title);
    elements_string_fit_width(canvas, s, 112);
    canvas_draw_str(canvas, 2, 9, furi_string_get_cstr(s));
    furi_string_free(s);
    if(m->connected) {
        canvas_draw_disc(canvas, 122, 5, 3);
    } else {
        canvas_draw_circle(canvas, 122, 5, 3);
    }
    canvas_draw_line(canvas, 0, 11, 128, 11);

    uint32_t now = furi_get_tick();
    bool flashing =
        (m->flash_index >= 0) && ((now - m->flash_tick) < furi_ms_to_ticks(ATV_FLASH_MS));

    for(int i = 0; i < ATV_BTN_COUNT; i++) {
        const AtvButton* b = &BUTTONS[i];
        bool sel = (i == m->selected);
        bool press = flashing && (i == m->flash_index);
        bool fill = sel || press;
        if(fill) {
            canvas_draw_rbox(canvas, b->x, b->y, b->w, b->h, 2);
            canvas_set_color(canvas, ColorWhite);
        } else {
            canvas_draw_rframe(canvas, b->x, b->y, b->w, b->h, 2);
        }
        canvas_draw_str_aligned(
            canvas, b->x + b->w / 2, b->y + b->h / 2 + 1, AlignCenter, AlignCenter, b->label);
        if(fill) canvas_set_color(canvas, ColorBlack);
    }
}

static bool atv_remote_input(InputEvent* event, void* context) {
    View* view = context;
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return false;

    if(event->key == InputKeyOk) {
        int keycode = -1;
        WlanAtvKeyCallback cb = NULL;
        void* cb_ctx = NULL;
        with_view_model(
            view,
            WlanAtvRemoteModel * m,
            {
                keycode = BUTTONS[m->selected].keycode;
                m->flash_index = m->selected;
                m->flash_tick = furi_get_tick();
                cb = m->cb;
                cb_ctx = m->cb_ctx;
            },
            true);
        if(keycode >= 0 && cb) cb(cb_ctx, keycode);
        return true;
    }

    // Linear (wrapping) traversal through every button — like the infrared
    // universal remote's button_panel. The view runs in ViewInputModeLeftRight,
    // so the T-Embed encoder's rotation arrives as Left/Right (hold+rotate as
    // Up/Down); both axes drive the same cycle so plain rotation reaches every
    // button without an OK-hold modifier. Touch boards send Left/Right/Up/Down
    // directly. CCW (Left/Down) = next, CW (Right/Up) = prev.
    int delta = 0;
    if(event->key == InputKeyLeft || event->key == InputKeyDown)
        delta = 1;
    else if(event->key == InputKeyRight || event->key == InputKeyUp)
        delta = -1;
    if(delta != 0) {
        with_view_model(
            view,
            WlanAtvRemoteModel * m,
            { m->selected = (uint8_t)((m->selected + ATV_BTN_COUNT + delta) % ATV_BTN_COUNT); },
            true);
        return true;
    }
    return false; // Back etc. -> scene manager
}

View* wlan_androidtv_remote_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanAtvRemoteModel));
    with_view_model(
        view,
        WlanAtvRemoteModel * m,
        {
            m->title[0] = '\0';
            m->connected = false;
            m->selected = ATV_BTN_OK;
            m->flash_index = -1;
            m->flash_tick = 0;
            m->cb = NULL;
            m->cb_ctx = NULL;
        },
        false);
    view_set_context(view, view);
    view_set_draw_callback(view, atv_remote_draw);
    view_set_input_callback(view, atv_remote_input);
    // LeftRight mode: the T-Embed encoder's rotation is remapped to Left/Right
    // so it linearly cycles the button selection (see the input callback).
    view_set_input_mode(view, ViewInputModeLeftRight);
    return view;
}

void wlan_androidtv_remote_view_free(View* view) {
    view_free(view);
}

void wlan_androidtv_remote_view_set_key_callback(
    View* view,
    WlanAtvKeyCallback callback,
    void* context) {
    with_view_model(
        view,
        WlanAtvRemoteModel * m,
        {
            m->cb = callback;
            m->cb_ctx = context;
        },
        false);
}

void wlan_androidtv_remote_view_set_status(View* view, const char* title, bool connected) {
    with_view_model(
        view,
        WlanAtvRemoteModel * m,
        {
            if(title) {
                strncpy(m->title, title, sizeof(m->title) - 1);
                m->title[sizeof(m->title) - 1] = '\0';
            }
            m->connected = connected;
        },
        true);
}
