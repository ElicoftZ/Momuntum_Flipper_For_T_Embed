#include "wlan_hal.h"
#include "wlan_passwords.h"

#include <esp_wifi.h>
#include <esp_private/wifi.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_event.h>
#include <esp_sntp.h>
#include <esp_log.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <furi.h>
#include <furi_hal_rtc.h>
#include <toolbox/saved_struct.h>
#include <sys/time.h>
#include <string.h>
#include <stdlib.h>
#include <btshim.h>
#include <nimble_glue.h>

#define TAG "WlanHal"
/* ESP-IDF's xTaskCreateStaticPinnedToCore() stack depth is BYTES, unlike
 * vanilla FreeRTOS. The old 4 KB stack overflowed during HTTPS. Twelve KB
 * leaves 3x that space while returning 4 KB of contiguous internal DMA RAM
 * needed to initialize WiFi alongside NimBLE and the HTTPD WebSocket code. */
#define WLAN_HAL_WORKER_STACK_BYTES (12U * 1024U)
#define WLAN_BOOT_TIME_SYNC_STACK 3072
#define WLAN_MANUAL_TIME_SYNC_STACK 3072
#define WLAN_MANUAL_TIME_SYNC_TIMEOUT_MS 20000U
#define WLAN_MANUAL_TIME_SYNC_POLL_MS 100U
#define WLAN_TIMEZONE_URL "https://worldtimeapi.org/api/ip"
#define WLAN_TIMEZONE_RESPONSE_MAX 1536U
#define WLAN_TIMEZONE_HTTP_TIMEOUT_MS 7000
#define WLAN_TIMEZONE_RETRY_MS (30U * 60U * 1000U)
#define WLAN_TIMEZONE_FALLBACK_REFRESH_SEC (7U * 24U * 60U * 60U)
#define WLAN_TIMEZONE_WORKER_POLL_MS (60U * 1000U)
#define WLAN_RADIO_SETTINGS_PATH "/int/.wlan_radio.settings"
#define WLAN_RADIO_SETTINGS_MAGIC 0x57
#define WLAN_RADIO_SETTINGS_VERSION 1
/* One-shot: skip WiFi's boot-time auto-reconnect on exactly the next boot,
 * without touching the persistent WlanRadioSettings.enabled above. Set right
 * before rebooting into freshly-flashed firmware so Bluetooth features have
 * full memory headroom immediately post-update; cleared as soon as it's read. */
#define WLAN_POST_UPDATE_HOLD_PATH "/int/.wifi_post_update_hold"
#define WLAN_POST_UPDATE_HOLD_MAGIC 0x50
#define WLAN_POST_UPDATE_HOLD_VERSION 1
/* The S3 hardware-AES driver uses two 1600-byte internal DMA bounce buffers
 * during a WPA2/WPA3 handshake. Keep a contiguous block before normal apps
 * fragment RAM, then release it immediately before association. 3584 bytes
 * covers both buffers and allocator metadata while still fitting the 3840-byte
 * largest block measured after Archive on a first boot. */
#define WLAN_AUTH_MEMORY_RESERVE_BYTES (3584U)
typedef struct {
    bool enabled;
} WlanRadioSettings;

typedef struct {
    bool hold;
} WlanPostUpdateHold;

typedef enum {
    WCMD_INIT_RESERVE,
    WCMD_INIT_START,
    WCMD_STOP_KEEP_INIT,
    WCMD_STOP_DEINIT,
    WCMD_SCAN,
    WCMD_CONNECT,
    WCMD_DISCONNECT,
    WCMD_SEND_ETH_RAW,
    WCMD_SET_CHANNEL,
    WCMD_SET_PROMISC,
    WCMD_SEND_RAW,
    WCMD_RUN_FN,
    WCMD_TIME_SYNC_START,
    WCMD_TIME_SYNC_COMPLETE,
    WCMD_TIMEZONE_SYNC,
    WCMD_QUIT,
} WlanCmdType;

typedef struct {
    WlanCmdType type;
    union {
        struct {
            wifi_scan_config_t* config;
            wifi_ap_record_t** out_records;
            uint16_t* out_count;
            uint16_t max_count;
        } scan;
        struct {
            char ssid[33];
            char password[65];
            uint8_t bssid[6];
            uint8_t channel;
            bool bssid_set;
        } connect;
        struct {
            uint8_t* buf;   // heap-alloziert vom Sender, free durch Consumer
            uint16_t len;
        } send_eth;
        struct {
            uint8_t channel;
        } set_channel;
        struct {
            bool enable;
            wifi_promiscuous_cb_t cb;
        } set_promisc;
        struct {
            uint8_t buf[64];
            uint16_t len;
        } send_raw;
        struct {
            WlanHalWorkerFn fn;
            void* arg;
        } run_fn;
        bool force_timezone_sync;
        uint64_t timestamp;
    };
    volatile bool* done;
    volatile bool* result;
} WlanCmd;


static bool s_started = false;
static bool s_bt_suspended = false;
static bool s_user_enabled = false;
static bool s_user_setting_loaded = false;
/* True for the rest of THIS boot only, from the moment the post-update hold
 * is consumed until WiFi actually starts for any reason (manual toggle, a
 * foreground app, etc.) - lets the status icon show "off" for a setting that
 * is enabled but deliberately not running yet. Not persisted; the persisted
 * one-shot flag itself is cleared the instant it's read, at boot. */
static bool s_post_update_held = false;
static volatile bool s_boot_time_sync_active = false;
static volatile bool s_boot_time_sync_cancel = false;
static volatile bool s_manual_time_sync_active = false;
static volatile bool s_manual_time_sync_cancel = false;
static volatile uint32_t s_time_sync_count = 0;
static bool s_netif_inited = false;
static esp_netif_t* s_netif_sta = NULL;
static volatile bool s_wifi_connected = false;
static volatile bool s_wifi_auto_reconnect = false;
static volatile uint8_t s_last_disconnect_reason = 0;
static volatile bool s_auth_fail_latched = false;

/** true für Disconnect-Reasons, die auf ein falsches Passwort / fehlgeschlagene
 *  Authentifizierung hindeuten. Bewusst OHNE „AP nicht gefunden"/Beacon-Timeout,
 *  damit ein korrektes gespeichertes Passwort bei einem bloßen Ausfall bleibt. */
static bool wlan_reason_is_auth(uint8_t reason) {
    switch(reason) {
    case WIFI_REASON_AUTH_EXPIRE: // 2
    case WIFI_REASON_MIC_FAILURE: // 14
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT: // 15 – klassisch: falsches WPA2-Passwort
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT: // 16
    case WIFI_REASON_IE_IN_4WAY_DIFFERS: // 17
    case WIFI_REASON_AUTH_FAIL: // 202
    case WIFI_REASON_HANDSHAKE_TIMEOUT: // 204
    case WIFI_REASON_CONNECTION_FAIL: // 205
        return true;
    default:
        return false;
    }
}
static bool s_event_handlers_registered = false;
static bool s_sntp_initialized = false;
static volatile bool s_time_sync_requested_for_connection = false;
static volatile bool s_time_sync_completion_queued = false;
static volatile uint32_t s_own_ip = 0;
static volatile uint32_t s_own_netmask = 0;
static bool s_timezone_attempted = false;
static TickType_t s_timezone_last_attempt_tick = 0;
static time_t s_timezone_next_refresh_epoch = 0;

static QueueHandle_t s_cmd_queue = NULL;
static TaskHandle_t s_worker_task = NULL;
static TaskHandle_t s_boot_time_sync_task = NULL;
static TaskHandle_t s_manual_time_sync_task = NULL;
static StackType_t* s_worker_stack = NULL;
static StaticTask_t s_worker_buf;
static void* s_auth_memory_reserve = NULL;
static portMUX_TYPE s_auth_memory_reserve_mux = portMUX_INITIALIZER_UNLOCKED;

static bool wlan_hal_connect_saved_network(void);
static bool wlan_hal_reserve_radio_memory(void);
static void wlan_hal_stop_internal(bool deinit_wifi);
static void wlan_hal_power_down(void);

static bool wlan_auth_memory_reserve(void) {
    portENTER_CRITICAL(&s_auth_memory_reserve_mux);
    const bool already_reserved = s_auth_memory_reserve != NULL;
    portEXIT_CRITICAL(&s_auth_memory_reserve_mux);
    if(already_reserved) return true;

    void* candidate = heap_caps_malloc(
        WLAN_AUTH_MEMORY_RESERVE_BYTES,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
    if(!candidate) {
        ESP_LOGW(
            TAG,
            "Could not reserve WPA auth memory: internal free=%u largest=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        return false;
    }

    portENTER_CRITICAL(&s_auth_memory_reserve_mux);
    if(!s_auth_memory_reserve) {
        s_auth_memory_reserve = candidate;
        candidate = NULL;
    }
    portEXIT_CRITICAL(&s_auth_memory_reserve_mux);
    if(candidate) heap_caps_free(candidate);
    return true;
}

static void wlan_auth_memory_release(void) {
    portENTER_CRITICAL(&s_auth_memory_reserve_mux);
    void* reserve = s_auth_memory_reserve;
    s_auth_memory_reserve = NULL;
    portEXIT_CRITICAL(&s_auth_memory_reserve_mux);
    if(reserve) heap_caps_free(reserve);
}

static void wlan_hal_load_user_setting(void) {
    if(s_user_setting_loaded) return;

    WlanRadioSettings settings = {.enabled = false};
    if(saved_struct_load(
           WLAN_RADIO_SETTINGS_PATH,
           &settings,
           sizeof(settings),
           WLAN_RADIO_SETTINGS_MAGIC,
           WLAN_RADIO_SETTINGS_VERSION)) {
        s_user_enabled = settings.enabled;
    } else {
        saved_struct_save(
            WLAN_RADIO_SETTINGS_PATH,
            &settings,
            sizeof(settings),
            WLAN_RADIO_SETTINGS_MAGIC,
            WLAN_RADIO_SETTINGS_VERSION);
    }
    s_user_setting_loaded = true;
}

static void wlan_hal_save_user_setting(void) {
    WlanRadioSettings settings = {.enabled = s_user_enabled};
    if(!saved_struct_save(
           WLAN_RADIO_SETTINGS_PATH,
           &settings,
           sizeof(settings),
           WLAN_RADIO_SETTINGS_MAGIC,
           WLAN_RADIO_SETTINGS_VERSION)) {
        ESP_LOGE(TAG, "Could not persist WiFi radio setting");
    }
}

void wlan_hal_hold_wifi_after_reboot(void) {
    WlanPostUpdateHold hold = {.hold = true};
    if(!saved_struct_save(
           WLAN_POST_UPDATE_HOLD_PATH,
           &hold,
           sizeof(hold),
           WLAN_POST_UPDATE_HOLD_MAGIC,
           WLAN_POST_UPDATE_HOLD_VERSION)) {
        ESP_LOGE(TAG, "Could not persist post-update WiFi hold");
    }
}

/* Returns true exactly once per hold - clears it immediately so a later boot
 * (or this one, if reserve/start fails and something retries) behaves
 * normally again. */
static bool wlan_hal_consume_post_update_hold(void) {
    WlanPostUpdateHold hold = {.hold = false};
    bool was_held = saved_struct_load(
                        WLAN_POST_UPDATE_HOLD_PATH,
                        &hold,
                        sizeof(hold),
                        WLAN_POST_UPDATE_HOLD_MAGIC,
                        WLAN_POST_UPDATE_HOLD_VERSION) &&
                    hold.hold;
    if(was_held) {
        hold.hold = false;
        saved_struct_save(
            WLAN_POST_UPDATE_HOLD_PATH,
            &hold,
            sizeof(hold),
            WLAN_POST_UPDATE_HOLD_MAGIC,
            WLAN_POST_UPDATE_HOLD_VERSION);
    }
    return was_held;
}

#define WLAN_TRUSTED_TIME_MIN 1704067200ULL /* 2024-01-01 UTC */

static const char* wlan_json_value(const char* json, const char* key) {
    char needle[32];
    const int needle_length = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if(needle_length <= 0 || (size_t)needle_length >= sizeof(needle)) return NULL;

    const char* value = strstr(json, needle);
    if(!value) return NULL;
    value = strchr(value + needle_length, ':');
    if(!value) return NULL;
    value++;
    while(*value == ' ' || *value == '\t' || *value == '\r' || *value == '\n') value++;
    return value;
}

static bool wlan_json_get_bool(const char* json, const char* key, bool* result) {
    const char* value = wlan_json_value(json, key);
    if(!value || !result) return false;
    if(strncmp(value, "true", 4) == 0) {
        *result = true;
        return true;
    }
    if(strncmp(value, "false", 5) == 0) {
        *result = false;
        return true;
    }
    return false;
}

static bool wlan_json_get_u64(const char* json, const char* key, uint64_t* result) {
    const char* value = wlan_json_value(json, key);
    if(!value || !result || *value < '0' || *value > '9') return false;

    uint64_t parsed = 0;
    while(*value >= '0' && *value <= '9') {
        const uint64_t digit = (uint64_t)(*value - '0');
        if(parsed > (UINT64_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
        value++;
    }
    *result = parsed;
    return true;
}

static bool wlan_json_get_string(
    const char* json,
    const char* key,
    char* result,
    size_t result_size) {
    const char* value = wlan_json_value(json, key);
    if(!value || !result || result_size == 0 || *value != '"') return false;
    value++;

    const char* end = strchr(value, '"');
    if(!end) return false;
    const size_t length = (size_t)(end - value);
    if(length == 0 || length >= result_size) return false;
    memcpy(result, value, length);
    result[length] = '\0';
    return true;
}

static bool wlan_parse_utc_offset(const char* json, int16_t* offset_minutes) {
    char offset[8];
    if(!wlan_json_get_string(json, "utc_offset", offset, sizeof(offset))) return false;
    if(strlen(offset) != 6 || (offset[0] != '+' && offset[0] != '-') ||
       offset[1] < '0' || offset[1] > '9' || offset[2] < '0' || offset[2] > '9' ||
       offset[3] != ':' || offset[4] < '0' || offset[4] > '9' || offset[5] < '0' ||
       offset[5] > '9') {
        return false;
    }

    const int hours = (offset[1] - '0') * 10 + (offset[2] - '0');
    const int minutes = (offset[4] - '0') * 10 + (offset[5] - '0');
    if(minutes >= 60 || hours > 14 || (hours == 14 && minutes != 0)) return false;

    int total = hours * 60 + minutes;
    if(offset[0] == '-') total = -total;
    if(total < -12 * 60 || total > 14 * 60) return false;
    *offset_minutes = (int16_t)total;
    return true;
}

static bool wlan_parse_two_digits(const char* text, int* result) {
    if(text[0] < '0' || text[0] > '9' || text[1] < '0' || text[1] > '9') return false;
    *result = (text[0] - '0') * 10 + (text[1] - '0');
    return true;
}

/* Gregorian civil date to days since 1970-01-01. This avoids changing the
 * process-wide TZ merely to parse WorldTimeAPI's UTC transition timestamp. */
static int64_t wlan_days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2U;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned year_of_era = (unsigned)(year - era * 400);
    const unsigned adjusted_month = month > 2U ? month - 3U : month + 9U;
    const unsigned day_of_year = (153U * adjusted_month + 2U) / 5U + day - 1U;
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return (int64_t)era * 146097 + (int64_t)day_of_era - 719468;
}

static bool wlan_parse_iso8601_epoch(const char* text, time_t* result) {
    if(!text || !result || strlen(text) < 20 || text[4] != '-' || text[7] != '-' ||
       text[10] != 'T' || text[13] != ':' || text[16] != ':') {
        return false;
    }

    int century;
    int year_suffix;
    int month;
    int day;
    int hour;
    int minute;
    int second;
    if(!wlan_parse_two_digits(text, &century) ||
       !wlan_parse_two_digits(text + 2, &year_suffix) ||
       !wlan_parse_two_digits(text + 5, &month) ||
       !wlan_parse_two_digits(text + 8, &day) ||
       !wlan_parse_two_digits(text + 11, &hour) ||
       !wlan_parse_two_digits(text + 14, &minute) ||
       !wlan_parse_two_digits(text + 17, &second)) {
        return false;
    }

    const int year = century * 100 + year_suffix;
    static const uint8_t days_per_month[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if(year < 1970 || month < 1 || month > 12 || day < 1 || hour > 23 || minute > 59 ||
       second > 60) {
        return false;
    }
    int max_day = days_per_month[month - 1];
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    if(month == 2 && leap) max_day++;
    if(day > max_day) return false;

    int offset_seconds = 0;
    if(text[19] == '+' || text[19] == '-') {
        int offset_hours;
        int offset_minutes;
        if(strlen(text) < 25 || text[22] != ':' ||
           !wlan_parse_two_digits(text + 20, &offset_hours) ||
           !wlan_parse_two_digits(text + 23, &offset_minutes) || offset_hours > 23 ||
           offset_minutes > 59) {
            return false;
        }
        offset_seconds = offset_hours * 3600 + offset_minutes * 60;
        if(text[19] == '-') offset_seconds = -offset_seconds;
    } else if(text[19] != 'Z') {
        return false;
    }

    const int64_t seconds =
        wlan_days_from_civil(year, (unsigned)month, (unsigned)day) * 86400LL +
        (int64_t)hour * 3600LL + (int64_t)minute * 60LL + second - offset_seconds;
    if(seconds < 0) return false;
    *result = (time_t)seconds;
    return true;
}

static void wlan_schedule_timezone_refresh(const char* response) {
    uint64_t server_epoch = 0;
    bool dst = false;
    char transition[40];
    time_t transition_epoch = 0;

    const bool has_server_epoch = wlan_json_get_u64(response, "unixtime", &server_epoch);
    const bool has_dst = wlan_json_get_bool(response, "dst", &dst);
    const bool has_transition = has_dst &&
                                wlan_json_get_string(
                                    response,
                                    dst ? "dst_until" : "dst_from",
                                    transition,
                                    sizeof(transition)) &&
                                wlan_parse_iso8601_epoch(transition, &transition_epoch);

    if(has_transition && (!has_server_epoch || transition_epoch > (time_t)server_epoch)) {
        /* Refresh just after the DST boundary so the displayed offset changes
         * without polling the public service several times every day. */
        s_timezone_next_refresh_epoch = transition_epoch + 120;
    } else if(has_server_epoch) {
        s_timezone_next_refresh_epoch =
            (time_t)(server_epoch + WLAN_TIMEZONE_FALLBACK_REFRESH_SEC);
    } else {
        s_timezone_next_refresh_epoch =
            time(NULL) + (time_t)WLAN_TIMEZONE_FALLBACK_REFRESH_SEC;
    }
}

static bool wlan_timezone_retry_ready(void) {
    if(!s_timezone_attempted) return true;
    return (TickType_t)(xTaskGetTickCount() - s_timezone_last_attempt_tick) >=
           pdMS_TO_TICKS(WLAN_TIMEZONE_RETRY_MS);
}

static bool wlan_sync_timezone_from_ip(bool force) {
    if(!s_wifi_connected || !furi_hal_rtc_get_timezone_auto()) return false;
    if(!force && !wlan_timezone_retry_ready()) return false;

    s_timezone_attempted = true;
    s_timezone_last_attempt_tick = xTaskGetTickCount();

    char response[WLAN_TIMEZONE_RESPONSE_MAX];
    bool success = false;
    esp_http_client_config_t config = {
        .url = WLAN_TIMEZONE_URL,
        .user_agent = "Momentum-T-Embed/1.0",
        .timeout_ms = WLAN_TIMEZONE_HTTP_TIMEOUT_MS,
        .buffer_size = 512,
        .buffer_size_tx = 256,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = false,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if(!client) {
        ESP_LOGW(TAG, "Timezone lookup: HTTP client allocation failed");
        return false;
    }

    do {
        esp_http_client_set_header(client, "Accept", "application/json");
        esp_err_t err = esp_http_client_open(client, 0);
        if(err != ESP_OK) {
            ESP_LOGW(TAG, "Timezone lookup connection failed: %s", esp_err_to_name(err));
            break;
        }

        const int64_t content_length = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if((content_length < 0 && !esp_http_client_is_chunked_response(client)) || status != 200 ||
           content_length >= (int64_t)sizeof(response)) {
            ESP_LOGW(
                TAG,
                "Timezone lookup rejected response: status=%d length=%lld",
                status,
                (long long)content_length);
            break;
        }

        const int read_length =
            esp_http_client_read_response(client, response, sizeof(response) - 1U);
        if(read_length <= 0 || read_length >= (int)sizeof(response) - 1) {
            ESP_LOGW(TAG, "Timezone lookup returned an empty or oversized body");
            break;
        }
        response[read_length] = '\0';

        int16_t offset_minutes;
        if(!wlan_parse_utc_offset(response, &offset_minutes)) {
            ESP_LOGW(TAG, "Timezone lookup response had no valid UTC offset");
            break;
        }

        /* The user can select a manual zone while HTTPS is in progress. Never
         * overwrite that explicit choice when the request finishes. */
        if(!furi_hal_rtc_get_timezone_auto()) break;
        furi_hal_rtc_set_timezone(true, offset_minutes);
        wlan_schedule_timezone_refresh(response);
        ESP_LOGI(
            TAG,
            "Timezone synchronized: UTC%c%02d:%02d",
            offset_minutes < 0 ? '-' : '+',
            abs(offset_minutes) / 60,
            abs(offset_minutes) % 60);
        success = true;
    } while(false);

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return success;
}

static void wlan_maybe_refresh_timezone(void) {
    if(!s_wifi_connected || !furi_hal_rtc_get_timezone_auto() || !wlan_timezone_retry_ready()) {
        return;
    }

    const time_t now = time(NULL);
    if(s_timezone_next_refresh_epoch == 0 ||
       ((uint64_t)now >= WLAN_TRUSTED_TIME_MIN && now >= s_timezone_next_refresh_epoch)) {
        wlan_sync_timezone_from_ip(false);
    }
}

static void wlan_time_sync_notification_cb(struct timeval* tv) {
    if(!tv || (uint64_t)tv->tv_sec < WLAN_TRUSTED_TIME_MIN || !s_cmd_queue ||
       !s_time_sync_requested_for_connection || s_time_sync_completion_queued) {
        return;
    }

    /* SNTP already applied this timestamp to the ESP system clock. Queue one
     * completion only; the worker immediately tears SNTP down so it cannot
     * perform periodic refreshes for the rest of this WiFi connection. */
    s_time_sync_completion_queued = true;
    WlanCmd cmd = {
        .type = WCMD_TIME_SYNC_COMPLETE,
        .timestamp = (uint64_t)tv->tv_sec,
    };
    if(xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        s_time_sync_completion_queued = false;
        ESP_LOGW(TAG, "Could not queue completed SNTP update");
    }
}

static bool wlan_request_time_sync(bool force) {
    if(!s_cmd_queue) return false;
    if(s_time_sync_requested_for_connection && !force) return true;

    if(force) s_time_sync_completion_queued = false;
    s_time_sync_requested_for_connection = true;
    WlanCmd cmd = {.type = WCMD_TIME_SYNC_START};
    if(xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        s_time_sync_requested_for_connection = false;
        ESP_LOGW(TAG, "Could not queue SNTP start");
        return false;
    }
    return true;
}

static void wlan_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    UNUSED(arg);
    if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t* event = event_data;
        const uint8_t reason = event ? event->reason : 0;
        wifi_event_sta_disconnected_t* d = (wifi_event_sta_disconnected_t*)event_data;
        if(d) {
            ESP_LOGW(TAG, "STA disconnected: reason=%u (ssid_len=%u)", (unsigned)d->reason, (unsigned)d->ssid_len);
            if(wlan_reason_is_auth((uint8_t)d->reason)) s_auth_fail_latched = true;
        }
        s_wifi_connected = false;
        s_own_ip = 0;
        s_own_netmask = 0;

        /* The timeout path asks the driver to leave after preserving the real
         * association failure. Do not replace an auth/no-AP reason with the
         * synthetic local-leave event produced by that cleanup. */
        const bool local_leave = !s_wifi_auto_reconnect &&
                                 (reason == WIFI_REASON_AUTH_LEAVE ||
                                  reason == WIFI_REASON_ASSOC_LEAVE ||
                                  reason == WIFI_REASON_STA_LEAVING);
        if(!local_leave || s_last_disconnect_reason == 0) {
            s_last_disconnect_reason = reason;
        }
        ESP_LOGW(
            TAG,
            "STA disconnected: reason=%u, saved_reason=%u, reconnect=%u",
            (unsigned)reason,
            (unsigned)s_last_disconnect_reason,
            (unsigned)s_wifi_auto_reconnect);
        s_time_sync_requested_for_connection = false;
        s_time_sync_completion_queued = false;
        if(s_wifi_auto_reconnect) {
            wlan_auth_memory_release();
            esp_wifi_connect();
        }
    } else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*)event_data;
        s_own_ip = event->ip_info.ip.addr;
        s_own_netmask = event->ip_info.netmask.addr;
        s_wifi_connected = true;
        s_last_disconnect_reason = 0;
        ESP_LOGI(
            TAG,
            "STA got IP: " IPSTR " gateway=" IPSTR,
            IP2STR(&event->ip_info.ip),
            IP2STR(&event->ip_info.gw));
        wlan_request_time_sync(false);
    }
}

static void wlan_worker_fn(void* arg) {
    UNUSED(arg);
    ESP_LOGI(TAG, "Worker started");

    WlanCmd cmd;
    while(1) {
        if(xQueueReceive(s_cmd_queue, &cmd, pdMS_TO_TICKS(WLAN_TIMEZONE_WORKER_POLL_MS)) !=
           pdTRUE) {
            wlan_maybe_refresh_timezone();
            continue;
        }

        bool ok = true;
        esp_err_t err;

        switch(cmd.type) {
        case WCMD_INIT_RESERVE:
        case WCMD_INIT_START:
            if(!s_netif_inited) {
                esp_netif_init();
                esp_event_loop_create_default();
                s_netif_sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
                if(!s_netif_sta) {
                    s_netif_sta = esp_netif_create_default_wifi_sta();
                }
                s_netif_inited = true;
            }
            if(!s_event_handlers_registered) {
                esp_event_handler_register(
                    WIFI_EVENT, ESP_EVENT_ANY_ID, &wlan_event_handler, NULL);
                esp_event_handler_register(
                    IP_EVENT, IP_EVENT_STA_GOT_IP, &wlan_event_handler, NULL);
                s_event_handlers_registered = true;
            }

            wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
            cfg.static_rx_buf_num = 2;
            cfg.dynamic_rx_buf_num = 4;
            cfg.dynamic_tx_buf_num = 8;

            err = esp_wifi_init(&cfg);
            if(err != ESP_OK && err != ESP_ERR_WIFI_INIT_STATE) {
                ESP_LOGE(TAG, "wifi_init: %s", esp_err_to_name(err));
                ok = false;
                break;
            }
            esp_wifi_set_storage(WIFI_STORAGE_RAM);
            if(cmd.type == WCMD_INIT_RESERVE) break;
            esp_wifi_set_mode(WIFI_MODE_STA);
            err = esp_wifi_start();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "wifi_start: %s", esp_err_to_name(err));
                ok = false;
            } else {
                /* Request the PHY's maximum TX power (84 = 21 dBm). The driver
                 * clamps this to CONFIG_ESP_PHY_MAX_WIFI_TX_POWER (20 dBm, the
                 * S3 ceiling). esp_wifi_start() already defaults to that ceiling,
                 * so this only guarantees no path left TX power reduced. */
                esp_wifi_set_max_tx_power(84);
            }
            break;

        case WCMD_STOP_KEEP_INIT:
        case WCMD_STOP_DEINIT:
            s_wifi_auto_reconnect = false;
            if(s_sntp_initialized) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
            }
            s_time_sync_requested_for_connection = false;
            s_time_sync_completion_queued = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            esp_wifi_set_promiscuous(false);
            esp_wifi_stop();
            if(cmd.type == WCMD_STOP_DEINIT) {
                err = esp_wifi_deinit();
                if(err != ESP_OK && err != ESP_ERR_WIFI_NOT_INIT) {
                    ESP_LOGW(TAG, "wifi_deinit: %s", esp_err_to_name(err));
                }
            } else {
                wlan_auth_memory_reserve();
            }
            break;

        case WCMD_CONNECT: {
            s_wifi_auto_reconnect = false;
            if(s_sntp_initialized) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
            }
            s_time_sync_requested_for_connection = false;
            s_time_sync_completion_queued = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            s_auth_fail_latched = false;
            vTaskDelay(pdMS_TO_TICKS(100));

            /* Return the contiguous DMA block to hardware AES only for the
             * handshake. This prevents correct WPA3 credentials from becoming
             * a false authentication failure under GUI/BLE memory pressure. */
            wlan_auth_memory_release();

            wifi_config_t wcfg = {0};
            strncpy((char*)wcfg.sta.ssid, cmd.connect.ssid, 32);
            if(cmd.connect.password[0]) {
                strncpy((char*)wcfg.sta.password, cmd.connect.password, 64);
                /* This is a minimum-security threshold, not a list of
                 * accepted modes. WPA_WPA2_PSK ranks above plain WPA2 in the
                 * IDF enum and therefore rejected ordinary WPA2 access points
                 * with WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY (210). */
                /* Accept legacy WPA as well as WPA2/WPA3.  This is only a
                 * minimum threshold; the selected AP and supplied passphrase
                 * still determine the negotiated security mode. */
                wcfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;
            } else {
                wcfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
            }
            if(cmd.connect.bssid_set) {
                wcfg.sta.bssid_set = true;
                memcpy(wcfg.sta.bssid, cmd.connect.bssid, 6);
            }
            if(cmd.connect.channel) {
                wcfg.sta.channel = cmd.connect.channel;
            }
            /* PMF-capable (but optional) supports modern WPA2/WPA3 transition
             * networks without excluding older WPA2 access points. */
            wcfg.sta.pmf_cfg.capable = true;
            wcfg.sta.pmf_cfg.required = false;
            /* A zero-initialized config leaves SAE PWE unspecified. Accept
             * both hunt-and-peck and hash-to-element so WPA3 and WPA2/WPA3
             * transition routers can negotiate with the same passphrase. */
            wcfg.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
            s_wifi_auto_reconnect = true;
            err = esp_wifi_set_config(WIFI_IF_STA, &wcfg);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "set_config: %s", esp_err_to_name(err));
                s_wifi_auto_reconnect = false;
                ok = false;
                break;
            }
            err = esp_wifi_connect();
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "connect: %s", esp_err_to_name(err));
                s_wifi_auto_reconnect = false;
                ok = false;
            }
            break;
        }

        case WCMD_DISCONNECT:
            s_wifi_auto_reconnect = false;
            if(s_sntp_initialized) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
            }
            s_time_sync_requested_for_connection = false;
            s_time_sync_completion_queued = false;
            esp_wifi_disconnect();
            s_wifi_connected = false;
            s_own_ip = 0;
            s_own_netmask = 0;
            break;

        case WCMD_SEND_ETH_RAW: {
            int eth_err = esp_wifi_internal_tx(WIFI_IF_STA, cmd.send_eth.buf, cmd.send_eth.len);
            if(eth_err != 0) {
                static uint32_t eth_err_count = 0;
                static uint32_t eth_err_last_log = 0;
                eth_err_count++;
                if(eth_err_count - eth_err_last_log >= 20) {
                    eth_err_last_log = eth_err_count;
                    ESP_LOGW(TAG, "internal_tx err=%d (count=%lu)",
                        eth_err, (unsigned long)eth_err_count);
                }
            }
            free(cmd.send_eth.buf);
            break;
        }

        case WCMD_SET_CHANNEL:
            esp_wifi_set_channel(cmd.set_channel.channel, WIFI_SECOND_CHAN_NONE);
            break;

        case WCMD_SET_PROMISC:
            // CB immer setzen (auch auf NULL), sonst bleibt ein zuvor
            // installierter RX-Callback aus einem anderen Subsystem aktiv.
            esp_wifi_set_promiscuous_rx_cb(cmd.set_promisc.enable ? cmd.set_promisc.cb : NULL);
            esp_wifi_set_promiscuous(cmd.set_promisc.enable);
            break;

        case WCMD_SEND_RAW: {
            // en_sys_seq=true: System füllt die Sequence-Number selbst → keine
            // duplizierten Frames mit fixem Seq aus dem Template.
            esp_err_t tx_err = esp_wifi_80211_tx(
                WIFI_IF_STA, cmd.send_raw.buf, cmd.send_raw.len, true);
            if(tx_err != ESP_OK) {
                ESP_LOGD(TAG, "80211_tx: %s", esp_err_to_name(tx_err));
            }
            break;
        }

        case WCMD_SCAN: {
            err = esp_wifi_scan_start(cmd.scan.config, true);
            if(err != ESP_OK) {
                ESP_LOGE(TAG, "scan: %s", esp_err_to_name(err));
                *cmd.scan.out_count = 0;
                *cmd.scan.out_records = NULL;
                ok = false;
                break;
            }
            uint16_t count = 0;
            err = esp_wifi_scan_get_ap_num(&count);
            if(err != ESP_OK) {
                esp_wifi_clear_ap_list();
                ok = false;
                break;
            }
            if(count > cmd.scan.max_count) count = cmd.scan.max_count;
            if(count > 0) {
                *cmd.scan.out_records = malloc(count * sizeof(wifi_ap_record_t));
                if(*cmd.scan.out_records) {
                    err = esp_wifi_scan_get_ap_records(&count, *cmd.scan.out_records);
                    if(err != ESP_OK) {
                        free(*cmd.scan.out_records);
                        *cmd.scan.out_records = NULL;
                        count = 0;
                        esp_wifi_clear_ap_list();
                        ok = false;
                    }
                } else {
                    count = 0;
                    esp_wifi_clear_ap_list();
                    ok = false;
                }
            } else {
                *cmd.scan.out_records = NULL;
                esp_wifi_clear_ap_list();
            }
            *cmd.scan.out_count = count;
            break;
        }

        case WCMD_RUN_FN:
            if(cmd.run_fn.fn) cmd.run_fn.fn(cmd.run_fn.arg);
            break;

        case WCMD_TIME_SYNC_START:
            if(!s_sntp_initialized) {
                esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
                config.wait_for_sync = false;
                config.sync_cb = wlan_time_sync_notification_cb;
                err = esp_netif_sntp_init(&config);
                if(err == ESP_OK) {
                    s_sntp_initialized = true;
                    ESP_LOGI(TAG, "SNTP started after WiFi obtained an IP");
                } else {
                    ESP_LOGE(TAG, "sntp_init: %s", esp_err_to_name(err));
                    s_time_sync_requested_for_connection = false;
                    ok = false;
                }
            }
            break;

        case WCMD_TIME_SYNC_COMPLETE:
            if(!s_time_sync_requested_for_connection) break;

            /* ESP-IDF has already written time(NULL). Dolphin/XP deliberately
             * does not consume this wall-clock value; its daily and inactivity
             * windows use the independent RTC elapsed counter. */
            if(s_sntp_initialized) {
                esp_netif_sntp_deinit();
                s_sntp_initialized = false;
            }
            furi_hal_rtc_sync_system_time();
            s_time_sync_completion_queued = false;
            s_time_sync_count++;
            ESP_LOGI(
                TAG,
                "One-shot time sync stored in ESP32 RTC: %llu",
                (unsigned long long)cmd.timestamp);
            break;

        case WCMD_TIMEZONE_SYNC:
            ok = wlan_sync_timezone_from_ip(cmd.force_timezone_sync);
            break;

        case WCMD_QUIT:
            ESP_LOGI(TAG, "Worker quitting");
            if(cmd.done) *cmd.done = true;
            vTaskDelete(NULL);
            return;
        }

        if(cmd.result) *cmd.result = ok;
        if(cmd.done) *cmd.done = true;
    }
}

static bool wlan_ensure_worker(void) {
    if(s_worker_task) return true;

    s_cmd_queue = xQueueCreate(4, sizeof(WlanCmd));
    if(!s_cmd_queue) return false;

    s_worker_stack = heap_caps_malloc(
        WLAN_HAL_WORKER_STACK_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!s_worker_stack) {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
        ESP_LOGE(TAG, "Cannot alloc worker stack");
        return false;
    }

    s_worker_task = xTaskCreateStaticPinnedToCore(
        wlan_worker_fn, "WlanWorker", WLAN_HAL_WORKER_STACK_BYTES,
        NULL, 5, s_worker_stack, &s_worker_buf, 0);
    return s_worker_task != NULL;
}

bool wlan_hal_ensure_worker(void) {
    return wlan_ensure_worker();
}

static void wlan_release_worker(void) {
    if(!s_worker_task && !s_cmd_queue && !s_worker_stack) return;

    /* The worker stack is deliberately internal RAM because its HTTPS/NVS
     * paths can run while the flash cache is disabled. Once WiFi is fully
     * stopped, keeping that 12 KB stack around prevents BLE HID from creating
     * its event task. Drop the idle worker here; wlan_ensure_worker() recreates
     * it on demand the next time WiFi is used. */
    TaskHandle_t worker = s_worker_task;
    QueueHandle_t queue = s_cmd_queue;
    StackType_t* stack = s_worker_stack;
    s_worker_task = NULL;
    s_cmd_queue = NULL;
    s_worker_stack = NULL;

    if(worker) vTaskDelete(worker);
    if(queue) vQueueDelete(queue);
    heap_caps_free(stack);
    memset(&s_worker_buf, 0, sizeof(s_worker_buf));

    ESP_LOGI(
        TAG,
        "Worker released: internal free=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
}

static void wlan_send_cmd_sync(WlanCmd* cmd) {
    volatile bool done = false;
    cmd->done = &done;
    xQueueSend(s_cmd_queue, cmd, portMAX_DELAY);
    while(!done) {
        furi_delay_ms(10);
    }
}

static bool wlan_suspend_ble_for_memory(void) {
    if(!nimble_glue_is_initialized() || !furi_record_exists(RECORD_BT)) return false;
    Bt* bt = furi_record_open(RECORD_BT);
    bt_stop_stack(bt);
    const bool stopped = !nimble_glue_is_initialized();
    if(stopped) s_bt_suspended = true;
    furi_record_close(RECORD_BT);
    ESP_LOGW(TAG, "BLE suspended=%u; internal free=%u largest=%u", stopped,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return stopped;
}

static void wlan_restore_ble(void) {
    if(!s_bt_suspended || !furi_record_exists(RECORD_BT)) return;
    s_bt_suspended = false;
    Bt* bt = furi_record_open(RECORD_BT);
    /* Respect a user who switched Bluetooth off during the WiFi session. */
    if(bt_is_enabled(bt)) bt_start_stack(bt);
    furi_record_close(RECORD_BT);
}

static bool wlan_start_attempt(void) {
    if(!wlan_ensure_worker()) return false;
    volatile bool result = false;
    WlanCmd cmd = {.type = WCMD_INIT_START, .result = &result};
    wlan_send_cmd_sync(&cmd);
    return result;
}

bool wlan_hal_is_held_after_update(void) {
    return s_post_update_held;
}

bool wlan_hal_start(void) {
    s_post_update_held = false;
    if(s_started) return true;
    /* An auth reserve cannot help if it prevents the driver from starting. */
    wlan_auth_memory_release();
    /* The two radios are mutually exclusive on this board. Leaving BLE up while
     * WiFi runs coexists two stacks in the same scarce internal DRAM, which
     * bleeds down until the power-service I2C poll cannot allocate a command
     * link and panics (StoreProhibited writing through a NULL i2c_cmd_handle).
     * Suspend BLE before the first start attempt, not just as a fallback — the
     * fallback below never ran when the first attempt happened to succeed, so
     * BLE stayed up and the coexistence starved the board. wlan_hal_power_down()
     * on the stop and failure paths calls wlan_restore_ble(), so BLE returns
     * when the WiFi session ends (unless the user turned Bluetooth off since). */
    wlan_suspend_ble_for_memory();
    bool result = wlan_start_attempt();
    if(!result) {
        ESP_LOGW(TAG, "WiFi start failed; internal free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
        /* Drop a partially initialized driver before retrying. */
        if(s_cmd_queue) wlan_hal_stop_internal(true);
        result = wlan_start_attempt();
    }

    if(result) {
        s_started = true;
        wlan_auth_memory_reserve();
        ESP_LOGI(TAG, "WiFi started; BLE suspended=%u", s_bt_suspended);
    } else {
        wlan_hal_power_down();
    }
    return result;
}

static bool wlan_hal_reserve_radio_memory(void) {
    if(!wlan_ensure_worker()) return false;

    volatile bool result = false;
    WlanCmd cmd = {.type = WCMD_INIT_RESERVE, .result = &result};
    wlan_send_cmd_sync(&cmd);
    if(result) {
        const bool auth_reserved = wlan_auth_memory_reserve();
        ESP_LOGI(
            TAG,
            "WiFi memory reserved: auth=%u internal free=%u largest=%u",
            (unsigned)auth_reserved,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
            (unsigned)heap_caps_get_largest_free_block(
                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }
    return result;
}

void wlan_hal_prepare_radio_memory(void) {
    wlan_hal_load_user_setting();

    if(wlan_hal_consume_post_update_hold()) {
        s_post_update_held = true;
        /* Persist WiFi off, not just for this one boot: after an OTA the user
         * wants Bluetooth to keep working across resets until they turn WiFi
         * back on themselves, instead of WiFi auto-returning on the next
         * reboot and starving BLE again. Turning it on from Control Centre
         * saves enabled=true again as normal. */
        if(s_user_enabled) {
            s_user_enabled = false;
            wlan_hal_save_user_setting();
        }
        ESP_LOGI(TAG, "Post-update boot: WiFi persisted off so Bluetooth keeps working");
        return;
    }

    /* Keep Wi‑Fi completely off until the user enables it from Control.
     * Reserving the worker stack and the driver's DMA pools while the switch
     * is off needlessly consumes the same internal RAM used by the display,
     * SD, and foreground apps.  The explicit Control action calls
     * wlan_hal_start() on demand, so no functionality is lost. */
    if(!s_user_enabled) {
        ESP_LOGI(TAG, "WiFi remains off until enabled from Control");
        return;
    }

    /* Reserve the driver's contiguous DMA pools and the command stack before
     * GUI/services fragment internal RAM. The radio itself stays stopped when
     * the user switch is off, so this does not transmit or contend with BLE.
     * Later scans can now start deterministically instead of depending on the
     * largest free block at the instant the foreground app opens. */
    if(!wlan_hal_reserve_radio_memory()) {
        ESP_LOGE(TAG, "Could not reserve WiFi memory during boot");
        if(s_user_enabled) {
            s_user_enabled = false;
            wlan_hal_save_user_setting();
        }
        wlan_hal_power_down();
        return;
    }

    if(!s_user_enabled || s_started) {
        return;
    }

    if(wlan_hal_start()) {
        wlan_hal_connect_saved_network();
        return;
    }

    ESP_LOGE(TAG, "Could not restore persistent WiFi during boot");
    s_user_enabled = false;
    wlan_hal_save_user_setting();
    wlan_hal_power_down();
}

bool wlan_hal_prepare_foreground_session(void) {
    return wlan_auth_memory_reserve();
}

static bool wlan_send_cmd_async(WlanCmd* cmd) {
    if(!s_cmd_queue) return false;
    cmd->done = NULL;
    cmd->result = NULL;
    return xQueueSend(s_cmd_queue, cmd, 0) == pdTRUE;
}

static void wlan_hal_power_down(void) {
    wlan_auth_memory_release();
    if(s_cmd_queue) {
        wlan_hal_stop_internal(true);
    } else {
        s_started = false;
    }
    wlan_release_worker();
    wlan_restore_ble();
}

static void wlan_hal_stop_internal(bool deinit_wifi) {
    if(deinit_wifi) wlan_auth_memory_release();
    if(s_started || deinit_wifi) {
        WlanCmd cmd = {
            .type = deinit_wifi ? WCMD_STOP_DEINIT : WCMD_STOP_KEEP_INIT,
        };
        wlan_send_cmd_sync(&cmd);
        if(s_started) {
            s_started = false;
            ESP_LOGI(TAG, "WiFi stopped%s", deinit_wifi ? " and released" : "");
        }
    }
}

static void wlan_hal_cancel_boot_time_sync(void) {
    if(!s_boot_time_sync_active) return;

    s_boot_time_sync_cancel = true;
    /* The boot task polls every 100 ms. Wait for it to release or hand over the
     * radio before a foreground app or lock-menu action touches that radio. */
    while(s_boot_time_sync_active) {
        furi_delay_ms(10);
    }
}

static void wlan_hal_cancel_manual_time_sync(void) {
    if(!s_manual_time_sync_active) return;

    s_manual_time_sync_cancel = true;
    while(s_manual_time_sync_active) {
        furi_delay_ms(10);
    }
}

bool wlan_hal_yield_for_memory(void) {
    wlan_hal_cancel_boot_time_sync();
    wlan_hal_cancel_manual_time_sync();
    /* Not gated on s_started: a caller may run after something already did a
     * weak stop_internal(false) (driver still init'd, memory still held), so
     * checking s_started here would silently no-op exactly when there's still
     * memory to free. Mirrors wlan_hal_power_down()'s own s_cmd_queue guard,
     * which is already safe to call unconditionally. Deliberately does not
     * touch BLE state - the caller doesn't take BLE here, it just needs the
     * memory; wlan_hal_resume_user_radio() is the matching resume call. */
    wlan_auth_memory_release();
    if(s_cmd_queue) {
        wlan_hal_stop_internal(true);
    } else {
        s_started = false;
    }
    wlan_release_worker();
    return true;
}

void wlan_hal_stop(void) {
    wlan_hal_cancel_boot_time_sync();
    wlan_hal_cancel_manual_time_sync();
    if(s_user_enabled) {
        /* Apps may leave STA disconnected, promiscuous, or pinned to an attack
         * channel. Normalize it back to a clean background STA and reconnect
         * the last successful network instead of preserving that attack mode. */
        ESP_LOGI(TAG, "Returning WiFi to background STA");
        wlan_hal_stop_internal(false);
        if(wlan_hal_start()) {
            wlan_hal_connect_saved_network();
        } else {
            s_user_enabled = false;
        }
        return;
    }
    if(s_bt_suspended) {
        wlan_hal_power_down();
        return;
    }
    /* Keep the boot-time allocation stable. Only the radio is stopped; BLE
     * stays active and the next WiFi app does not need a large contiguous
     * allocation after the GUI and application stacks have fragmented RAM. */
    wlan_hal_stop_internal(false);
}

void wlan_hal_finish_foreground_session(void) {
    wlan_hal_cancel_boot_time_sync();
    wlan_hal_cancel_manual_time_sync();

    if(s_user_enabled) {
        /* Do not restart here: wlan_app still owns its views, scan records and
         * credential buffers. Desktop's AfterAppFinished callback reconnects
         * once the loader has freed the foreground thread and its stack. */
        ESP_LOGI(TAG, "Foreground WiFi stopped; deferring background reconnect");
        wlan_hal_stop_internal(false);
    } else {
        if(s_bt_suspended) {
            wlan_hal_stop_internal(true);
            wlan_release_worker();
            /* Desktop restores BLE after the app's views and stack are freed. */
        } else {
            wlan_hal_stop_internal(false);
        }
    }
}

void wlan_hal_stop_for_reconfigure(void) {
    wlan_hal_cancel_boot_time_sync();
    wlan_hal_cancel_manual_time_sync();
    wlan_hal_stop_internal(true);
    /* Keep the 12 KB internal worker stack reserved while another subsystem
     * owns the WiFi driver. It will reserve the driver again after app exit. */
}

bool wlan_hal_set_user_enabled(bool enabled) {
    wlan_hal_load_user_setting();
    if(enabled) {
        /* Setting this first makes a running boot sync hand its live STA over
         * to the user instead of tearing WiFi down as it exits. */
        s_user_enabled = true;
        wlan_hal_save_user_setting();
        wlan_hal_cancel_boot_time_sync();
        wlan_hal_cancel_manual_time_sync();
        if(s_user_enabled && s_started) {
            return true;
        }
        if(!wlan_hal_start()) {
            s_user_enabled = false;
            wlan_hal_save_user_setting();
            wlan_hal_power_down();
            return false;
        }
        wlan_hal_connect_saved_network();
        return true;
    }

    const bool was_enabled = s_user_enabled;
    s_user_enabled = false;
    if(was_enabled) wlan_hal_save_user_setting();
    wlan_hal_cancel_boot_time_sync();
    wlan_hal_cancel_manual_time_sync();
    if(s_bt_suspended) wlan_hal_power_down();
    else wlan_hal_stop_internal(false);
    return true;
}

bool wlan_hal_is_user_enabled(void) {
    wlan_hal_load_user_setting();
    return s_user_enabled;
}

void wlan_hal_suspend_user_radio(void) {
    wlan_hal_cancel_boot_time_sync();
    wlan_hal_cancel_manual_time_sync();
    if(s_user_enabled) {
        wlan_hal_stop_internal(false);
        /* The foreground WiFi app reuses this worker. Releasing and reallocating
         * its contiguous internal stack after the app loads is not reliable. */
    }
}

bool wlan_hal_resume_user_radio(void) {
    if(!s_user_enabled) {
        wlan_restore_ble();
        wlan_hal_prepare_radio_memory();
        return true;
    }
    /* This runs every time any app closes (desktop's AfterAppFinished), which
     * would otherwise silently restart WiFi the moment the user closes their
     * first app post-update - defeating the whole point of the hold after
     * one navigation. Let it expire on its own; any deliberate
     * wlan_hal_start() (manual toggle, a foreground app) clears it. */
    if(wlan_hal_is_held_after_update()) return true;
    /* A manual sync which started WiFi while Settings was open will leave the
     * connected STA running when the persistent user switch is enabled. */
    if(s_manual_time_sync_active) return true;
    if(s_started) {
        if(!s_wifi_connected && !s_wifi_auto_reconnect) wlan_hal_connect_saved_network();
        return true;
    }
    if(wlan_hal_start()) {
        wlan_hal_connect_saved_network();
        return true;
    }

    /* The switch must describe reality, so clear a preference whose driver
     * could not be started. Bluetooth remains independent throughout. */
    s_user_enabled = false;
    return false;
}

void wlan_hal_set_bt_restore(bool restore) {
    /* NimBLE and WiFi coexist on this port; neither owns the other radio. */
    UNUSED(restore);
}

bool wlan_hal_is_started(void) {
    return s_started;
}

bool wlan_hal_connect(const char* ssid, const char* password, const uint8_t* bssid, uint8_t channel) {
    if(!s_started || !ssid) return false;

    /* Do not let a reason from the previous attempt make the UI discard a
     * password from this one. The event handler fills this only after a real
     * disconnect. */
    s_last_disconnect_reason = 0;

    WlanCmd cmd = {.type = WCMD_CONNECT};
    memset(&cmd.connect, 0, sizeof(cmd.connect));
    strncpy(cmd.connect.ssid, ssid, 32);
    if(password && password[0]) {
        strncpy(cmd.connect.password, password, 64);
    }
    if(bssid) {
        memcpy(cmd.connect.bssid, bssid, 6);
        cmd.connect.bssid_set = true;
    }
    cmd.connect.channel = channel;

    /* Association is asynchronous by nature.  Do not make the GUI application's
     * event loop wait on esp_wifi_disconnect/set_config/connect; the scene polls
     * the IP state and disconnect reason already. */
    if(!wlan_send_cmd_async(&cmd)) {
        ESP_LOGE(TAG, "connect: worker queue full");
        return false;
    }
    return true;
}

uint8_t wlan_hal_get_last_disconnect_reason(void) {
    return s_last_disconnect_reason;
}

static bool wlan_hal_connect_saved_network(void) {
    char ssid[33] = {0};
    char password[65] = {0};

    if(!wlan_last_ssid_read(ssid, sizeof(ssid))) {
        ESP_LOGI(TAG, "Background WiFi: no last SSID saved yet");
        return false;
    }

    /* A missing password means an open network. Protected networks connected
     * through the WiFi app already have their per-SSID password file. */
    wlan_password_read(ssid, password, sizeof(password));
    ESP_LOGI(TAG, "Background WiFi: connecting to '%s'", ssid);
    return wlan_hal_connect(ssid, password, NULL, 0);
}

static void wlan_boot_time_sync_task_fn(void* context) {
    UNUSED(context);

    bool connect_started = false;

    /* Only restore WiFi when the user left its persistent switch enabled.
     * A disabled switch no longer borrows the BLE radio merely to set time. */
    if(s_user_enabled && !s_boot_time_sync_cancel && wlan_hal_start()) {
        if(!s_boot_time_sync_cancel) {
            connect_started = wlan_hal_connect_saved_network();
        }
        if(connect_started) {
            ESP_LOGI(TAG, "Persistent WiFi restored after reboot");
        }
    }

    if(s_user_enabled) {
        ESP_LOGI(TAG, "Boot WiFi handed over to the user switch");
    } else {
        wlan_hal_stop_internal(false);
    }

    s_boot_time_sync_cancel = false;
    s_boot_time_sync_task = NULL;
    s_boot_time_sync_active = false;
    vTaskDelete(NULL);
}

void wlan_hal_start_boot_time_sync(void) {
    wlan_hal_load_user_setting();
    if(s_boot_time_sync_active || s_boot_time_sync_task || s_started) return;

    /* Time is synchronized once by the normal IP-acquired path. Do not start
     * WiFi solely for timekeeping when the user's WiFi switch is off. */
    if(!s_user_enabled) return;

    s_boot_time_sync_cancel = false;
    s_boot_time_sync_active = true;
    if(xTaskCreate(
           wlan_boot_time_sync_task_fn,
           "BootTimeSync",
           WLAN_BOOT_TIME_SYNC_STACK,
           NULL,
           4,
           &s_boot_time_sync_task) != pdPASS) {
        s_boot_time_sync_task = NULL;
        s_boot_time_sync_active = false;
        ESP_LOGE(TAG, "Could not create boot time-sync task");
    }
}

bool wlan_hal_is_boot_time_sync_active(void) {
    return s_boot_time_sync_active;
}

static void wlan_manual_time_sync_task_fn(void* context) {
    UNUSED(context);

    const bool started_here = !s_started;
    const uint32_t sync_count_before = s_time_sync_count;
    uint32_t waited_ms = 0;
    bool connect_started = false;

    if(!s_manual_time_sync_cancel && (s_started || wlan_hal_start())) {
        if(s_wifi_connected) {
            connect_started = wlan_request_time_sync(true);
        } else if(!s_manual_time_sync_cancel) {
            connect_started = wlan_hal_connect_saved_network();
        }

        while(connect_started && !s_manual_time_sync_cancel && s_started &&
              s_time_sync_count == sync_count_before &&
              waited_ms < WLAN_MANUAL_TIME_SYNC_TIMEOUT_MS) {
            furi_delay_ms(WLAN_MANUAL_TIME_SYNC_POLL_MS);
            waited_ms += WLAN_MANUAL_TIME_SYNC_POLL_MS;
        }

        if(s_time_sync_count != sync_count_before) {
            ESP_LOGI(TAG, "Manual time sync complete");
        } else if(!s_manual_time_sync_cancel) {
            ESP_LOGW(TAG, "Manual time sync timed out");
        }
    }

    /* When Settings temporarily started WiFi, retain it only if the user's
     * persistent WiFi switch is enabled. Bluetooth is never modified. */
    if(started_here && !s_user_enabled) {
        wlan_hal_stop_internal(false);
    }

    s_manual_time_sync_cancel = false;
    s_manual_time_sync_task = NULL;
    s_manual_time_sync_active = false;
    vTaskDelete(NULL);
}

bool wlan_hal_start_manual_time_sync(void) {
    if(s_manual_time_sync_active || s_manual_time_sync_task) return true;

    if(s_started && s_wifi_connected) return wlan_request_time_sync(true);

    char saved_ssid[33] = {0};
    if(!wlan_last_ssid_read(saved_ssid, sizeof(saved_ssid))) {
        ESP_LOGW(TAG, "Manual time sync needs a saved WiFi network");
        return false;
    }

    s_manual_time_sync_cancel = false;
    s_manual_time_sync_active = true;
    if(xTaskCreate(
           wlan_manual_time_sync_task_fn,
           "ManualTimeSync",
           WLAN_MANUAL_TIME_SYNC_STACK,
           NULL,
           4,
           &s_manual_time_sync_task) != pdPASS) {
        s_manual_time_sync_task = NULL;
        s_manual_time_sync_active = false;
        ESP_LOGE(TAG, "Could not create manual time-sync task");
        return false;
    }
    return true;
}

void wlan_hal_disconnect(void) {
    if(!s_started) return;
    WlanCmd cmd = {.type = WCMD_DISCONNECT};
    wlan_send_cmd_sync(&cmd);
}

bool wlan_hal_is_connected(void) {
    return s_wifi_connected;
}

bool wlan_hal_disconnect_async(void) {
    if(!s_started) return true;
    WlanCmd cmd = {.type = WCMD_DISCONNECT};
    if(!wlan_send_cmd_async(&cmd)) {
        ESP_LOGW(TAG, "disconnect: worker queue full");
        return false;
    }
    return true;
}

void wlan_hal_request_timezone_refresh(void) {
    if(!s_wifi_connected || !s_cmd_queue || !furi_hal_rtc_get_timezone_auto()) return;

    WlanCmd cmd = {
        .type = WCMD_TIMEZONE_SYNC,
        .force_timezone_sync = true,
    };
    if(xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Could not queue timezone refresh");
    }
}

typedef struct {
    wifi_ap_record_t* out;
    bool result;
} WlanGetApArg;

/* Läuft im wlan-Worker-Task: esp_wifi_sta_get_ap_info() ist ioctl-basiert und
 * nutzt pthread-TLS des WiFi-Treibers — ein Aufruf aus einem FuriThread (z.B.
 * der WiFi-App) crasht (LoadProhibited). Daher via wlan_hal_run_in_worker. */
static void wlan_get_ap_worker(void* p) {
    WlanGetApArg* a = p;
    a->result = (esp_wifi_sta_get_ap_info(a->out) == ESP_OK);
}

bool wlan_hal_get_connected_ap(wifi_ap_record_t* out) {
    if(!s_started || !s_wifi_connected || !out) return false;
    WlanGetApArg arg = {.out = out, .result = false};
    if(!wlan_hal_run_in_worker(wlan_get_ap_worker, &arg)) return false;
    return arg.result;
}

bool wlan_hal_last_fail_is_auth(void) {
    return s_auth_fail_latched;
}

uint32_t wlan_hal_get_own_ip(void) {
    return s_own_ip;
}

uint32_t wlan_hal_get_netmask(void) {
    return s_own_netmask;
}

bool wlan_hal_get_own_mac(uint8_t out[6]) {
    if(!s_started) return false;
    return esp_wifi_get_mac(WIFI_IF_STA, out) == ESP_OK;
}

uint32_t wlan_hal_get_gw_ip(void) {
    if(!s_netif_sta) return 0;
    esp_netif_ip_info_t info;
    if(esp_netif_get_ip_info(s_netif_sta, &info) != ESP_OK) return 0;
    return info.gw.addr;
}

bool wlan_hal_send_eth_raw(const uint8_t* data, uint16_t len) {
    if(!s_started || !s_cmd_queue) {
        ESP_LOGW(TAG, "send_eth_raw: not started (s=%d q=%p)", s_started, s_cmd_queue);
        return false;
    }
    if(!data || len < 14 || len > 1600) return false;
    uint8_t* buf = malloc(len);
    if(!buf) {
        ESP_LOGE(TAG, "send_eth_raw: malloc(%u) failed", (unsigned)len);
        return false;
    }
    memcpy(buf, data, len);
    WlanCmd cmd = {.type = WCMD_SEND_ETH_RAW, .done = NULL, .result = NULL};
    cmd.send_eth.buf = buf;
    cmd.send_eth.len = len;
    if(xQueueSend(s_cmd_queue, &cmd, 0) != pdTRUE) {
        static uint32_t qfull_count = 0;
        static uint32_t qfull_last_log = 0;
        qfull_count++;
        if(qfull_count - qfull_last_log >= 20) {
            qfull_last_log = qfull_count;
            ESP_LOGW(TAG, "send_eth_raw: cmd queue full (count=%lu)",
                (unsigned long)qfull_count);
        }
        free(buf);
        return false;
    }
    return true;
}

void wlan_hal_set_channel(uint8_t channel) {
    if(!s_started || channel < 1 || channel > 14) return;
    WlanCmd cmd = {.type = WCMD_SET_CHANNEL, .set_channel = {.channel = channel}};
    wlan_send_cmd_sync(&cmd);
}

void wlan_hal_set_promiscuous(bool enable, wifi_promiscuous_cb_t cb) {
    if(!s_started) return;
    WlanCmd cmd = {.type = WCMD_SET_PROMISC, .set_promisc = {.enable = enable, .cb = cb}};
    wlan_send_cmd_sync(&cmd);
}

bool wlan_hal_send_raw(const uint8_t* data, uint16_t len) {
    if(!s_started || !s_cmd_queue || len > 64) return false;
    WlanCmd cmd = {.type = WCMD_SEND_RAW, .done = NULL, .result = NULL};
    memcpy(cmd.send_raw.buf, data, len);
    cmd.send_raw.len = len;
    return xQueueSend(s_cmd_queue, &cmd, 0) == pdTRUE;
}

bool wlan_hal_raw_tx_retry(const uint8_t* data, uint16_t len) {
    // Direkter TX aus dem aufrufenden Task (NICHT über den Worker-Queue-
    // Roundtrip wie wlan_hal_send_raw) — für High-Rate-Deauth-Bursts. Bei
    // vollem TX-Ring (ESP_ERR_NO_MEM) kurz warten und erneut versuchen, sonst
    // gehen im Burst still Frames verloren. en_sys_seq=true → HW füllt die Seq.
    esp_err_t err = esp_wifi_80211_tx(WIFI_IF_STA, data, len, true);
    for(int i = 0; err == ESP_ERR_NO_MEM && i < 3; i++) {
        vTaskDelay(1); // TX-Ring drainen lassen
        err = esp_wifi_80211_tx(WIFI_IF_STA, data, len, true);
    }
    return err == ESP_OK;
}

bool wlan_hal_run_in_worker(WlanHalWorkerFn fn, void* arg) {
    if(!fn) return false;
    // Lazy-init the worker queue + task. Evil Portal is entered directly from
    // the menu without going through wlan_hal_start (no STA scan), so the
    // worker may not exist yet. Safe to call repeatedly; no-ops if already up.
    if(!wlan_ensure_worker()) return false;
    WlanCmd cmd = {.type = WCMD_RUN_FN, .run_fn = {.fn = fn, .arg = arg}};
    wlan_send_cmd_sync(&cmd);
    return true;
}

bool wlan_hal_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count) {
    if(!out_records || !out_count) return false;
    *out_records = NULL;
    *out_count = 0;
    if(!s_started || max_count == 0) return false;
    volatile bool result = false;
    wifi_scan_config_t scan_config = {
        .ssid = NULL, .bssid = NULL, .channel = 0,
        .show_hidden = true, .scan_type = WIFI_SCAN_TYPE_ACTIVE,
    };
    WlanCmd cmd = {
        .type = WCMD_SCAN,
        .result = &result,
        .scan = {
            .config = &scan_config,
            .out_records = out_records,
            .out_count = out_count,
            .max_count = max_count,
        },
    };
    wlan_send_cmd_sync(&cmd);
    return result;
}

// ---------------------------------------------------------------------------
// Beacon-Spam: dedizierter xTaskCreate-Task (parallel zum Worker), nutzt
// esp_wifi_80211_tx() direkt. Frame-Counter + Stop-Flag sind volatile.
// ---------------------------------------------------------------------------

static const uint8_t beacon_packet_template[] = {
    0x80, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x83, 0x51, 0xf7, 0x8f, 0x0f, 0x00,
    0x00, 0x00, 0xe8, 0x03, 0x31, 0x00, 0x00, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x01, 0x08,
    0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c, 0x03, 0x01,
    0x01, 0x30, 0x18, 0x01, 0x00, 0x00, 0x0f, 0xac, 0x02, 0x02,
    0x00, 0x00, 0x0f, 0xac, 0x04, 0x00, 0x0f, 0xac, 0x04, 0x01,
    0x00, 0x00, 0x0f, 0xac, 0x02, 0x00, 0x00,
};

static const char* beacon_funny_ssids[] = {
    "Mom Use This One", "Abraham Linksys", "Benjamin FrankLAN",
    "Martin Router King", "John Wilkes Bluetooth", "Pretty Fly for a Wi-Fi",
    "Bill Wi the Science Fi", "I Believe Wi Can Fi", "Tell My Wi-Fi Love Her",
    "No More Mister Wi-Fi", "LAN Solo", "The LAN Before Time",
    "Silence of the LANs", "House LANister", "Winternet Is Coming",
    "FBI Surveillance Van 4", "Area 51 Test Site", "Never Gonna Give You Up",
    "Loading...", "VIRUS.EXE", "Free Public Wi-Fi", "404 Wi-Fi Unavailable",
    NULL,
};

static const char* beacon_rickroll_ssids[] = {
    "01 Never gonna give you up",
    "02 Never gonna let you down",
    "03 Never gonna run around",
    "04 And desert you",
    "05 Never gonna make you cry",
    "06 Never gonna say goodbye",
    "07 Never gonna tell a lie",
    "08 And hurt you",
    NULL,
};

static volatile bool s_beacon_active = false;
static volatile uint32_t s_beacon_frames = 0;
static TaskHandle_t s_beacon_task = NULL;
static WlanHalBeaconMode s_beacon_mode = WlanHalBeaconModeFunny;
static char s_beacon_base_ssid[33] = {0};

static void prepare_beacon_packet(uint8_t* packet, const uint8_t* mac,
                                  const char* ssid, uint8_t channel) {
    memcpy(packet, beacon_packet_template, sizeof(beacon_packet_template));
    memcpy(&packet[10], mac, 6);
    memcpy(&packet[16], mac, 6);
    uint8_t ssid_len = (uint8_t)strlen(ssid);
    if(ssid_len > 32) ssid_len = 32;
    packet[37] = ssid_len;
    memcpy(&packet[38], ssid, ssid_len);
    packet[82] = channel;
}

static void beacon_spam_task(void* param) {
    (void)param;
    const char** ssid_list = NULL;
    char gen_ssid[64];
    uint8_t mac[6];
    uint8_t packet[sizeof(beacon_packet_template)];
    int ssid_index = 0;
    int counter = 1;
    uint8_t channel = 1;

    srand((unsigned)esp_log_timestamp());

    // Promiscuous (cb=NULL) erlaubt 80211_tx auf STA-Interface.
    esp_wifi_set_promiscuous(true);

    while(s_beacon_active) {
        const char* current_ssid = NULL;
        switch(s_beacon_mode) {
        case WlanHalBeaconModeFunny:
            ssid_list = beacon_funny_ssids;
            current_ssid = ssid_list[ssid_index++];
            if(ssid_list[ssid_index] == NULL) ssid_index = 0;
            break;
        case WlanHalBeaconModeRickroll:
            ssid_list = beacon_rickroll_ssids;
            current_ssid = ssid_list[ssid_index++];
            if(ssid_list[ssid_index] == NULL) ssid_index = 0;
            break;
        case WlanHalBeaconModeRandom:
            snprintf(gen_ssid, sizeof(gen_ssid), "SSID_%d", rand() % 9999);
            current_ssid = gen_ssid;
            break;
        case WlanHalBeaconModeCustom:
            if(s_beacon_base_ssid[0]) {
                snprintf(gen_ssid, sizeof(gen_ssid), "%s%d",
                    s_beacon_base_ssid, counter++);
                if(counter > 9999) counter = 1;
            } else {
                snprintf(gen_ssid, sizeof(gen_ssid), "SSID_%d", rand() % 9999);
            }
            current_ssid = gen_ssid;
            break;
        }
        if(!current_ssid) current_ssid = "Flipper";

        // Random Locally-Administered Unicast MAC.
        for(int i = 0; i < 6; i++) mac[i] = rand() & 0xFF;
        mac[0] = (mac[0] & 0xFE) | 0x02;

        prepare_beacon_packet(packet, mac, current_ssid, channel);
        if(esp_wifi_80211_tx(WIFI_IF_STA, packet,
               sizeof(beacon_packet_template), false) == ESP_OK) {
            s_beacon_frames++;
        }

        if((s_beacon_frames % 5) == 0) {
            channel++;
            if(channel > 11) channel = 1;
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_wifi_set_promiscuous(false);
    s_beacon_task = NULL;
    vTaskDelete(NULL);
}

void wlan_hal_beacon_spam_start(WlanHalBeaconMode mode, const char* base_ssid) {
    if(s_beacon_active || s_beacon_task) return;
    if(!s_started) {
        if(!wlan_hal_start()) return;
    }
    if(s_wifi_connected) wlan_hal_disconnect();

    s_beacon_mode = mode;
    memset(s_beacon_base_ssid, 0, sizeof(s_beacon_base_ssid));
    if(mode == WlanHalBeaconModeCustom && base_ssid) {
        strncpy(s_beacon_base_ssid, base_ssid, sizeof(s_beacon_base_ssid) - 1);
    }
    s_beacon_frames = 0;
    s_beacon_active = true;
    BaseType_t rc = xTaskCreate(beacon_spam_task, "BeaconSpam",
        4096, NULL, 5, &s_beacon_task);
    if(rc != pdPASS) {
        s_beacon_active = false;
        s_beacon_task = NULL;
        ESP_LOGE(TAG, "beacon_spam: xTaskCreate failed");
    }
}

void wlan_hal_beacon_spam_stop(void) {
    if(!s_beacon_active && !s_beacon_task) return;
    s_beacon_active = false;
    // Task beendet sich selbst (vTaskDelete im Task). Auf Beendigung warten.
    while(s_beacon_task) vTaskDelay(pdMS_TO_TICKS(10));
}

bool wlan_hal_beacon_spam_is_running(void) {
    return s_beacon_active;
}

uint32_t wlan_hal_beacon_spam_get_frame_count(void) {
    return s_beacon_frames;
}

// ---------------------------------------------------------------------------
// Probe-Request-Flood: spoofed-source-MAC probe requests for random SSIDs,
// the Marauder-style "probe flood" attack. Distinct from beacon spam (which
// impersonates an AP); this impersonates many CLIENTS searching for networks,
// which is what floods an AP's/IDS's association table and probe-response
// handling rather than showing up in a station's own AP scan list.
// ---------------------------------------------------------------------------

static const uint8_t probe_req_packet_template[] = {
    // 802.11 header (24 bytes): FC = mgmt/probe-request, dur=0,
    // addr1 (RA/DA) = broadcast, addr2 (TA/SA) = placeholder (overwritten
    // per-frame with a random MAC), addr3 (BSSID) = broadcast, seq-ctl=0.
    0x40, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00,
    // SSID IE: tag=0x00, len placeholder (overwritten with the actual
    // length), 32-byte slot pre-filled with spaces -- same trick as the
    // beacon-spam template, so a shorter SSID doesn't leave stray NUL bytes
    // ahead of the next IE.
    0x00, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,
    // Supported Rates IE.
    0x01, 0x08, 0x82, 0x84, 0x8b, 0x96, 0x24, 0x30, 0x48, 0x6c,
};

static volatile bool s_probe_flood_active = false;
static volatile uint32_t s_probe_flood_frames = 0;
static TaskHandle_t s_probe_flood_task = NULL;

static void prepare_probe_req_packet(uint8_t* packet, const uint8_t* mac, const char* ssid) {
    memcpy(packet, probe_req_packet_template, sizeof(probe_req_packet_template));
    memcpy(&packet[10], mac, 6); // addr2 / TA -- addr3/BSSID stays broadcast
    uint8_t ssid_len = (uint8_t)strlen(ssid);
    if(ssid_len > 32) ssid_len = 32;
    packet[25] = ssid_len;
    memcpy(&packet[26], ssid, ssid_len);
}

static void probe_flood_task(void* param) {
    (void)param;
    char gen_ssid[16];
    uint8_t mac[6];
    uint8_t packet[sizeof(probe_req_packet_template)];
    uint8_t channel = 1;

    srand((unsigned)esp_log_timestamp());

    // Promiscuous (cb=NULL) erlaubt 80211_tx auf STA-Interface (wie beim
    // Beacon-Spam-Task).
    esp_wifi_set_promiscuous(true);

    while(s_probe_flood_active) {
        for(int i = 0; i < 6; i++) mac[i] = rand() & 0xFF;
        mac[0] = (mac[0] & 0xFE) | 0x02; // random locally-administered unicast MAC

        snprintf(gen_ssid, sizeof(gen_ssid), "SSID_%d", rand() % 9999);

        prepare_probe_req_packet(packet, mac, gen_ssid);
        if(esp_wifi_80211_tx(WIFI_IF_STA, packet,
               sizeof(probe_req_packet_template), false) == ESP_OK) {
            s_probe_flood_frames++;
        }

        if((s_probe_flood_frames % 5) == 0) {
            channel++;
            if(channel > 11) channel = 1;
            esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }

    esp_wifi_set_promiscuous(false);
    s_probe_flood_task = NULL;
    vTaskDelete(NULL);
}

void wlan_hal_probe_flood_start(void) {
    if(s_probe_flood_active || s_probe_flood_task) return;
    if(!s_started) {
        if(!wlan_hal_start()) return;
    }
    if(s_wifi_connected) wlan_hal_disconnect();

    s_probe_flood_frames = 0;
    s_probe_flood_active = true;
    BaseType_t rc = xTaskCreate(probe_flood_task, "ProbeFlood",
        4096, NULL, 5, &s_probe_flood_task);
    if(rc != pdPASS) {
        s_probe_flood_active = false;
        s_probe_flood_task = NULL;
        ESP_LOGE(TAG, "probe_flood: xTaskCreate failed");
    }
}

void wlan_hal_probe_flood_stop(void) {
    if(!s_probe_flood_active && !s_probe_flood_task) return;
    s_probe_flood_active = false;
    // Task beendet sich selbst (vTaskDelete im Task). Auf Beendigung warten.
    while(s_probe_flood_task) vTaskDelay(pdMS_TO_TICKS(10));
}

bool wlan_hal_probe_flood_is_running(void) {
    return s_probe_flood_active;
}

uint32_t wlan_hal_probe_flood_get_frame_count(void) {
    return s_probe_flood_frames;
}
