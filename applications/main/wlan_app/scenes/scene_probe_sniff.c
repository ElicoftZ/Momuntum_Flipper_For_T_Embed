#include "../wlan_app.h"
#include "../wlan_probe.h"
#include "../wlan_pcap_rec.h"
#include <wlan_hal.h>
#include <furi_hal.h>
#include <esp_wifi.h>
#include <esp_timer.h>
#include <gui/elements.h>
#include <freertos/FreeRTOS.h>

#define PROBE_ROWS 64
#define PROBE_EVENT_TOGGLE 1
#define PROBE_EVENT_CHANNEL 2
#define PROBE_EVENT_PREV 3
#define PROBE_EVENT_NEXT 4

typedef struct {
    WlanProbe probe;
    uint32_t count;
    uint32_t last_seen;
    int8_t rssi;
    uint8_t channel;
} ProbeRow;

typedef struct {
    ProbeRow rows[PROBE_ROWS];
    uint32_t received;
    uint32_t overflow;
    uint8_t count;
    uint8_t selected;
    uint8_t channel;
    uint8_t channel_min;
    uint8_t channel_max;
    bool hopping;
    bool running;
    bool saving;
    char status[32];
    View* view;
    WlanApp* app;
} ProbeSniff;

/* The driver only updates bounded records. Rendering and SD writes run
 * outside this lock; detaching under it protects the callback on scene exit. */
static portMUX_TYPE probe_lock = portMUX_INITIALIZER_UNLOCKED;
static ProbeSniff* probe_sniff;

static void probe_receive(void* buffer, wifi_promiscuous_pkt_type_t type) {
    if(type != WIFI_PKT_MGMT || !buffer) return;
    const wifi_promiscuous_pkt_t* packet = buffer;
    if(packet->rx_ctrl.sig_len < 4 || packet->rx_ctrl.rx_state != 0) return;
    WlanProbe probe;
    if(!wlan_probe_parse(packet->payload, packet->rx_ctrl.sig_len - 4, &probe)) return;
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000000);
    bool save = false;
    portENTER_CRITICAL(&probe_lock);
    ProbeSniff* s = probe_sniff;
    if(s && s->running) {
        ++s->received;
        save = s->saving;
        unsigned index = 0;
        for(; index < s->count; ++index) {
            WlanProbe* old = &s->rows[index].probe;
            if(!memcmp(old->mac, probe.mac, 6) && old->ssid_length == probe.ssid_length &&
               !memcmp(old->ssid, probe.ssid, probe.ssid_length)) break;
        }
        if(index < PROBE_ROWS) {
            ProbeRow* row = &s->rows[index];
            if(index == s->count) { row->probe = probe; ++s->count; }
            ++row->count;
            row->last_seen = now;
            row->channel = packet->rx_ctrl.channel;
            row->rssi = packet->rx_ctrl.rssi;
        } else {
            ++s->overflow;
        }
    }
    portEXIT_CRITICAL(&probe_lock);
    if(save) wlan_pcap_rec_frame(packet->payload, packet->rx_ctrl.sig_len);
}

static void probe_draw(Canvas* canvas, void* context) {
    ProbeSniff* s = context;
    ProbeRow row = {0};
    uint8_t count, selected;
    uint32_t received, overflow;
    portENTER_CRITICAL(&probe_lock);
    count = s->count;
    selected = s->selected;
    if(count) row = s->rows[selected];
    received = s->received;
    overflow = s->overflow;
    portEXIT_CRITICAL(&probe_lock);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontSecondary);
    char line[64];
    snprintf(line, sizeof(line), "Probe %s Ch%u %lu", s->hopping ? "Auto" : "Hold",
             s->channel, (unsigned long)received);
    canvas_draw_str(canvas, 1, 8, line);
    if(count) {
        char ssid[33];
        for(unsigned i = 0; i < row.probe.ssid_length; ++i) {
            uint8_t c = row.probe.ssid[i];
            ssid[i] = c >= 32 && c < 127 ? (char)c : '.';
        }
        ssid[row.probe.ssid_length] = 0;
        char first[22];
        snprintf(first, sizeof(first), "%.21s", row.probe.ssid_length ? ssid : "<wildcard probe>");
        canvas_draw_str(canvas, 1, 18, first);
        if(row.probe.ssid_length > 21) canvas_draw_str(canvas, 1, 26, ssid + 21);
        snprintf(line, sizeof(line), "%02X:%02X:%02X:%02X:%02X:%02X",
                 row.probe.mac[0], row.probe.mac[1], row.probe.mac[2],
                 row.probe.mac[3], row.probe.mac[4], row.probe.mac[5]);
        canvas_draw_str(canvas, 1, 35, line);
        snprintf(line, sizeof(line), "%ddBm Ch%u x%lu %lus ago", row.rssi, row.channel,
                 (unsigned long)row.count,
                 (unsigned long)((uint32_t)(esp_timer_get_time() / 1000000) - row.last_seen));
        canvas_draw_str(canvas, 1, 44, line);
    } else {
        canvas_draw_str(canvas, 1, 24, "OK: start passive capture");
        canvas_draw_str(canvas, 1, 37, "Up/Down: browse devices");
    }
    if(s->status[0]) {
        canvas_draw_str(canvas, 1, 53, s->status);
    } else {
        snprintf(line, sizeof(line), "%u/%u %s:%lu Drop:%lu%s", count ? selected + 1 : 0,
                 count, s->saving ? "REC" : "Saved", (unsigned long)wlan_pcap_rec_frames(),
                 (unsigned long)wlan_pcap_rec_drops(), overflow ? " List full" : "");
        canvas_draw_str(canvas, 1, 53, line);
    }
    elements_button_right(canvas, s->running ? "Stop" : "Sniff");
}

static bool probe_input(InputEvent* input, void* context) {
    ProbeSniff* s = context;
    bool repeat = input->type == InputTypeRepeat;
    if(input->type != InputTypeShort && !repeat) return false;
    uint32_t event;
    switch(input->key) {
    /* Only browsing auto-repeats; toggles must not thrash on a held key. */
    case InputKeyUp: event = PROBE_EVENT_PREV; break;
    case InputKeyDown: event = PROBE_EVENT_NEXT; break;
    case InputKeyOk:
    case InputKeyRight:
        if(repeat) return false;
        event = PROBE_EVENT_TOGGLE;
        break;
    case InputKeyLeft:
        if(repeat) return false;
        event = PROBE_EVENT_CHANNEL;
        break;
    default: return false;
    }
    view_dispatcher_send_custom_event(s->app->view_dispatcher, event);
    return true;
}

static void probe_stop(ProbeSniff* s) {
    wlan_hal_set_promiscuous(false, NULL);
    portENTER_CRITICAL(&probe_lock);
    s->running = false;
    s->saving = false;
    portEXIT_CRITICAL(&probe_lock);
    wlan_pcap_rec_stop();
}

static void probe_draw_model(Canvas* canvas, void* model) {
    probe_draw(canvas, *(ProbeSniff**)model);
}

void wlan_app_scene_probe_sniff_on_enter(void* context) {
    WlanApp* app = context;
    ProbeSniff* s = calloc(1, sizeof(*s));
    furi_check(s);
    s->app = app;
    s->channel = 1;
    s->channel_min = 1;
    s->channel_max = 11;
    s->hopping = true;
    s->view = view_alloc();
    view_set_context(s->view, s);
    view_allocate_model(s->view, ViewModelTypeLockFree, sizeof(ProbeSniff*));
    *(ProbeSniff**)view_get_model(s->view) = s;
    view_commit_model(s->view, false);
    view_set_input_callback(s->view, probe_input);
    portENTER_CRITICAL(&probe_lock);
    probe_sniff = s;
    portEXIT_CRITICAL(&probe_lock);
    view_dispatcher_add_view(app->view_dispatcher, WlanAppViewProbeSniff, s->view);
    view_set_draw_callback(s->view, probe_draw_model);
    view_dispatcher_switch_to_view(app->view_dispatcher, WlanAppViewProbeSniff);
}

bool wlan_app_scene_probe_sniff_on_event(void* context, SceneManagerEvent event) {
    WlanApp* app = context;
    ProbeSniff* s = probe_sniff;
    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == PROBE_EVENT_TOGGLE) {
            if(s->running) {
                probe_stop(s);
            } else {
                if(!wlan_hal_is_started() && !wlan_hal_start()) {
                    strlcpy(s->status, "WiFi start failed", sizeof(s->status));
                    view_commit_model(s->view, true);
                    return true;
                }
                if(wlan_hal_is_connected()) wlan_hal_disconnect();
                app->connected = false;
                app->lan_scan_complete = false;
                wifi_country_t country;
                if(esp_wifi_get_country(&country) == ESP_OK && country.nchan) {
                    s->channel = country.schan;
                    s->channel_min = country.schan;
                    s->channel_max = country.schan + country.nchan - 1;
                    if(s->channel_max > 14) s->channel_max = 14;
                }
                wlan_hal_set_channel(s->channel);
                DateTime dt;
                furi_hal_rtc_get_datetime(&dt);
                char path[112];
                snprintf(path, sizeof(path), "/ext/wifi/probes/%04u%02u%02u_%02u%02u%02u_%lu.pcap",
                         dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second,
                         (unsigned long)(esp_timer_get_time() / 1000));
                bool saving = wlan_pcap_rec_start(path);
                strlcpy(s->status, saving ? "" : "Recording unavailable: live only", sizeof(s->status));
                portENTER_CRITICAL(&probe_lock);
                s->saving = saving;
                s->running = true;
                portEXIT_CRITICAL(&probe_lock);
                wlan_hal_set_promiscuous(true, probe_receive);
                bool enabled = false;
                if(esp_wifi_get_promiscuous(&enabled) != ESP_OK || !enabled) {
                    probe_stop(s);
                    strlcpy(s->status, "Capture start failed", sizeof(s->status));
                }
            }
        } else if(event.event == PROBE_EVENT_CHANNEL) {
            s->hopping = !s->hopping;
        } else if(event.event == PROBE_EVENT_PREV || event.event == PROBE_EVENT_NEXT) {
            portENTER_CRITICAL(&probe_lock);
            if(s->count) s->selected = event.event == PROBE_EVENT_PREV ?
                (s->selected ? s->selected - 1 : s->count - 1) : (s->selected + 1) % s->count;
            portEXIT_CRITICAL(&probe_lock);
        } else return false;
    } else if(event.type == SceneManagerEventTypeTick) {
        if(s->running && s->hopping) {
            s->channel = s->channel >= s->channel_max ? s->channel_min : s->channel + 1;
            wlan_hal_set_channel(s->channel);
        }
    } else return false;
    view_commit_model(s->view, true);
    return true;
}

void wlan_app_scene_probe_sniff_on_exit(void* context) {
    WlanApp* app = context;
    ProbeSniff* s = probe_sniff;
    probe_stop(s);
    portENTER_CRITICAL(&probe_lock);
    probe_sniff = NULL;
    portEXIT_CRITICAL(&probe_lock);
    view_dispatcher_remove_view(app->view_dispatcher, WlanAppViewProbeSniff);
    view_free(s->view);
    free(s);
}
