#include "wlan_fw_update_view.h"
#include "wlan_view_events.h"
#include <gui/canvas.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <input/input.h>
#include <string.h>
#include <stdio.h>

static void wlan_fw_update_view_draw_callback(Canvas* canvas, void* _model) {
    WlanFwUpdateViewModel* m = _model;
    canvas_clear(canvas);

    // Kopfzeile: Speed links, Titel zentriert.
    canvas_set_font(canvas, FontSecondary);
    if(m->speed_kbps > 0) {
        char sbuf[16];
        snprintf(sbuf, sizeof(sbuf), "%lu kB/s", (unsigned long)m->speed_kbps);
        canvas_draw_str_aligned(canvas, 2, 10, AlignLeft, AlignBottom, sbuf);
    }
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 10, AlignCenter, AlignBottom, "FW UPDATE");

    // Trennlinie volle Breite.
    canvas_draw_line(canvas, 0, 13, 127, 13);

    // Status.
    canvas_set_font(canvas, FontSecondary);
    canvas_draw_str_aligned(
        canvas, 64, 27, AlignCenter, AlignBottom, m->status[0] ? m->status : "");

    // Fortschrittsbalken volle Breite + KB-Text.
    char ptxt[24];
    if(m->bytes_total > 0) {
        snprintf(
            ptxt, sizeof(ptxt), "%lu/%lu KB",
            (unsigned long)(m->bytes_done / 1024u),
            (unsigned long)(m->bytes_total / 1024u));
    } else {
        snprintf(ptxt, sizeof(ptxt), "%u%%", m->percent);
    }
    float p = m->percent > 100 ? 1.0f : (float)m->percent / 100.0f;
    elements_progress_bar_with_text(canvas, 4, 32, 120, p, ptxt);

    // Hinweiszeile.
    canvas_draw_str_aligned(
        canvas, 64, 52, AlignCenter, AlignBottom, "Do not power off");

    // Cancel-Button (nur sinnvoll während Download/Check; die Scene ignoriert
    // Cancel während des Flashens).
    elements_button_left(canvas, "Cancel");
}

static bool wlan_fw_update_view_input_callback(InputEvent* event, void* context) {
    ViewDispatcher* vd = context;
    if(event->type != InputTypeShort) return false;
    if(event->key == InputKeyUp) {
        view_dispatcher_send_custom_event(vd, WlanAppCustomEventFwUpdateCancel);
        return true;
    }
    return false;
}

View* wlan_fw_update_view_alloc(void) {
    View* view = view_alloc();
    view_allocate_model(view, ViewModelTypeLocking, sizeof(WlanFwUpdateViewModel));
    view_set_draw_callback(view, wlan_fw_update_view_draw_callback);
    view_set_input_callback(view, wlan_fw_update_view_input_callback);
    return view;
}

void wlan_fw_update_view_free(View* view) {
    view_free(view);
}

void wlan_fw_update_view_update(
    View* view,
    const char* status,
    uint8_t percent,
    uint32_t bytes_done,
    uint32_t bytes_total,
    uint32_t speed_kbps) {
    WlanFwUpdateViewModel* m = view_get_model(view);
    strncpy(m->status, status ? status : "", sizeof(m->status) - 1);
    m->status[sizeof(m->status) - 1] = '\0';
    m->percent = percent;
    m->bytes_done = bytes_done;
    m->bytes_total = bytes_total;
    m->speed_kbps = speed_kbps;
    view_commit_model(view, true);
}
