#include "hotspot_arcade_runtime.h"

#include <furi.h>
#include <storage/storage.h>

#include <esp_err.h>
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_wifi.h>
#include <esp_wifi_default.h>

#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <lwip/inet.h>
#include <lwip/ip4_addr.h>
#include <lwip/sockets.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "HotspotArcadeRt"

#define HOTSPOT_ARCADE_WEB_PATH_MAX       255U
#define HOTSPOT_ARCADE_WEB_FILE_MAX       (4U * 1024U * 1024U)
#define HOTSPOT_ARCADE_CLIENT_CAPACITY    8U
#define HOTSPOT_ARCADE_WORKER_STACK       8192U
#define HOTSPOT_ARCADE_DNS_STACK          4096U
#define HOTSPOT_ARCADE_DNS_PORT           53
#define HOTSPOT_ARCADE_OUT_FRAGMENT_SIZE  4096U
#define HOTSPOT_ARCADE_SEND_TIMEOUT_RETRY 3U

#if defined(CONFIG_HTTPD_WS_SUPPORT) && CONFIG_HTTPD_WS_SUPPORT
#define HOTSPOT_ARCADE_HAS_WEBSOCKET 1
#else
#define HOTSPOT_ARCADE_HAS_WEBSOCKET 0
#endif

typedef enum {
    RuntimeStateStopped = 0,
    RuntimeStateStarting,
    RuntimeStateRunning,
    RuntimeStateStopping,
    /* The worker is suspended after teardown and still must be deleted by API. */
    RuntimeStateQuiescent,
} RuntimeState;

typedef struct {
    int fd;
    bool used;
    uint8_t* inbound;
    size_t inbound_length;
    bool inbound_text_fragment;
} RuntimeClient;

typedef struct {
    char ssid[33];
    char web_gzip_path[HOTSPOT_ARCADE_WEB_PATH_MAX + 1U];
    void* callback_context;
    HotspotArcadeRuntimeWsOpenCallback on_ws_open;
    HotspotArcadeRuntimeWsCloseCallback on_ws_close;
    HotspotArcadeRuntimeWsTextCallback on_ws_text;
    uint16_t max_ws_message_size;
    uint8_t channel;
    uint8_t max_connections;

    RuntimeState state;
    HotspotArcadeRuntimeResult start_result;
    SemaphoreHandle_t start_done;
    SemaphoreHandle_t stop_done;
    TaskHandle_t worker_task;

    bool wifi_initialized;
    bool wifi_started;
    esp_netif_t* ap_netif;
    httpd_handle_t http;

    uint8_t* web_gzip;
    size_t web_gzip_length;

    TaskHandle_t dns_task;
    SemaphoreHandle_t dns_ready;
    SemaphoreHandle_t dns_stopped;
    bool dns_run;
    bool dns_start_ok;
    int dns_socket;

    bool callbacks_enabled;
    size_t callbacks_inflight;
    RuntimeClient clients[HOTSPOT_ARCADE_CLIENT_CAPACITY];

    bool tx_accepting;
    size_t tx_enqueuers;
    size_t tx_work_count;
} RuntimeSession;

static RuntimeSession runtime = {
    .state = RuntimeStateStopped,
    .dns_socket = -1,
};
static portMUX_TYPE runtime_lock = portMUX_INITIALIZER_UNLOCKED;

static void* runtime_alloc(size_t size) {
    void* result = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!result) result = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    return result;
}

static void* runtime_realloc(void* pointer, size_t size) {
    void* result =
        heap_caps_realloc(pointer, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!result) result = heap_caps_realloc(pointer, size, MALLOC_CAP_8BIT);
    return result;
}

static void runtime_clear_client(RuntimeClient* client) {
    if(client->inbound) free(client->inbound);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
}

static RuntimeClient* runtime_find_client_locked(int fd) {
    for(size_t i = 0; i < HOTSPOT_ARCADE_CLIENT_CAPACITY; ++i) {
        if(runtime.clients[i].used && runtime.clients[i].fd == fd) return &runtime.clients[i];
    }
    return NULL;
}

static size_t runtime_connected_count_locked(void) {
    size_t count = 0;
    for(size_t i = 0; i < HOTSPOT_ARCADE_CLIENT_CAPACITY; ++i) {
        if(runtime.clients[i].used) ++count;
    }
    return count;
}

static void runtime_callback_open(int fd) {
    HotspotArcadeRuntimeWsOpenCallback callback = NULL;
    void* context = NULL;

    portENTER_CRITICAL(&runtime_lock);
    if(runtime.callbacks_enabled && runtime.on_ws_open) {
        callback = runtime.on_ws_open;
        context = runtime.callback_context;
        ++runtime.callbacks_inflight;
    }
    portEXIT_CRITICAL(&runtime_lock);

    if(callback) {
        callback(context, fd);
        portENTER_CRITICAL(&runtime_lock);
        --runtime.callbacks_inflight;
        portEXIT_CRITICAL(&runtime_lock);
    }
}

static void runtime_callback_close(int fd) {
    HotspotArcadeRuntimeWsCloseCallback callback = NULL;
    void* context = NULL;

    portENTER_CRITICAL(&runtime_lock);
    if(runtime.callbacks_enabled && runtime.on_ws_close) {
        callback = runtime.on_ws_close;
        context = runtime.callback_context;
        ++runtime.callbacks_inflight;
    }
    portEXIT_CRITICAL(&runtime_lock);

    if(callback) {
        callback(context, fd);
        portENTER_CRITICAL(&runtime_lock);
        --runtime.callbacks_inflight;
        portEXIT_CRITICAL(&runtime_lock);
    }
}

static void runtime_callback_text(int fd, const char* text, size_t length) {
    HotspotArcadeRuntimeWsTextCallback callback = NULL;
    void* context = NULL;

    portENTER_CRITICAL(&runtime_lock);
    if(runtime.callbacks_enabled && runtime.on_ws_text) {
        callback = runtime.on_ws_text;
        context = runtime.callback_context;
        ++runtime.callbacks_inflight;
    }
    portEXIT_CRITICAL(&runtime_lock);

    if(callback) {
        callback(context, fd, text, length);
        portENTER_CRITICAL(&runtime_lock);
        --runtime.callbacks_inflight;
        portEXIT_CRITICAL(&runtime_lock);
    }
}

static HotspotArcadeRuntimeResult runtime_load_web_file(void) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    if(!storage) return HotspotArcadeRuntimeResultStorageError;

    File* file = storage_file_alloc(storage);
    if(!file) {
        furi_record_close(RECORD_STORAGE);
        return HotspotArcadeRuntimeResultNoMemory;
    }

    HotspotArcadeRuntimeResult result = HotspotArcadeRuntimeResultStorageError;
    if(storage_file_open(file, runtime.web_gzip_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const uint64_t size64 = storage_file_size(file);
        if(size64 > 0 && size64 <= HOTSPOT_ARCADE_WEB_FILE_MAX) {
            const size_t size = (size_t)size64;
            const size_t largest_psram =
                heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            ESP_LOGI(
                TAG,
                "web=%u bytes, largest PSRAM block=%u",
                (unsigned)size,
                (unsigned)largest_psram);
            uint8_t* data =
                heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(data) {
                size_t total = 0;
                while(total < size) {
                    size_t chunk = size - total;
                    if(chunk > 4096U) chunk = 4096U;
                    const size_t read = storage_file_read(file, data + total, chunk);
                    if(read == 0) break;
                    total += read;
                }
                if(total == size) {
                    runtime.web_gzip = data;
                    runtime.web_gzip_length = size;
                    result = HotspotArcadeRuntimeResultOk;
                } else {
                    free(data);
                }
            } else {
                ESP_LOGE(
                    TAG,
                    "web bundle requires PSRAM: web=%u bytes, largest PSRAM block=%u",
                    (unsigned)size,
                    (unsigned)largest_psram);
                result = HotspotArcadeRuntimeResultNoMemory;
            }
        }
        storage_file_close(file);
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return result;
}

static bool runtime_dns_make_response(uint8_t* packet, size_t* packet_length) {
    const size_t input_length = *packet_length;
    if(input_length < 12U) return false;

    const uint16_t flags = ((uint16_t)packet[2] << 8U) | packet[3];
    const uint16_t question_count = ((uint16_t)packet[4] << 8U) | packet[5];
    if((flags & 0x8000U) != 0 || question_count == 0) return false;

    size_t cursor = 12U;
    while(cursor < input_length) {
        const uint8_t label_length = packet[cursor++];
        if(label_length == 0) break;
        if((label_length & 0xC0U) != 0 || label_length > 63U) return false;
        if(cursor + label_length > input_length) return false;
        cursor += label_length;
    }
    if(cursor + 4U > input_length) return false;

    const uint16_t question_type = ((uint16_t)packet[cursor] << 8U) | packet[cursor + 1U];
    const uint16_t question_class =
        ((uint16_t)packet[cursor + 2U] << 8U) | packet[cursor + 3U];
    cursor += 4U;

    const bool answer =
        (question_class == 1U) && (question_type == 1U || question_type == 255U);
    const size_t answer_length = answer ? 16U : 0U;
    if(cursor + answer_length > 512U) return false;

    /* Authoritative, recursion-available, no-error wildcard response. */
    packet[2] = 0x85U;
    packet[3] = 0x80U;
    packet[4] = 0;
    packet[5] = 1;
    packet[6] = 0;
    packet[7] = answer ? 1U : 0U;
    packet[8] = packet[9] = packet[10] = packet[11] = 0;

    if(answer) {
        uint8_t* out = packet + cursor;
        out[0] = 0xC0U;
        out[1] = 0x0CU;
        out[2] = 0;
        out[3] = 1;
        out[4] = 0;
        out[5] = 1;
        out[6] = 0;
        out[7] = 0;
        out[8] = 0;
        out[9] = 60;
        out[10] = 0;
        out[11] = 4;
        out[12] = 192;
        out[13] = 168;
        out[14] = 4;
        out[15] = 1;
    }

    *packet_length = cursor + answer_length;
    return true;
}

static void runtime_dns_task(void* context) {
    (void)context;
    int socket_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bool ready = false;

    if(socket_fd >= 0) {
        struct sockaddr_in address = {0};
        address.sin_family = AF_INET;
        address.sin_port = htons(HOTSPOT_ARCADE_DNS_PORT);
        address.sin_addr.s_addr = htonl(INADDR_ANY);

        int reuse = 1;
        setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        if(bind(socket_fd, (struct sockaddr*)&address, sizeof(address)) == 0) {
            struct timeval timeout = {.tv_sec = 0, .tv_usec = 200000};
            setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
            ready = true;
        }
    }

    portENTER_CRITICAL(&runtime_lock);
    runtime.dns_socket = socket_fd;
    runtime.dns_start_ok = ready;
    portEXIT_CRITICAL(&runtime_lock);
    xSemaphoreGive(runtime.dns_ready);

    uint8_t packet[512];
    while(ready) {
        portENTER_CRITICAL(&runtime_lock);
        const bool keep_running = runtime.dns_run;
        portEXIT_CRITICAL(&runtime_lock);
        if(!keep_running) break;

        struct sockaddr_in source = {0};
        socklen_t source_length = sizeof(source);
        const int received = recvfrom(
            socket_fd,
            packet,
            sizeof(packet),
            0,
            (struct sockaddr*)&source,
            &source_length);
        if(received <= 0) continue;

        size_t response_length = (size_t)received;
        if(runtime_dns_make_response(packet, &response_length)) {
            sendto(
                socket_fd,
                packet,
                response_length,
                0,
                (struct sockaddr*)&source,
                source_length);
        }
    }

    if(socket_fd >= 0) close(socket_fd);
    portENTER_CRITICAL(&runtime_lock);
    runtime.dns_socket = -1;
    portEXIT_CRITICAL(&runtime_lock);
    xSemaphoreGive(runtime.dns_stopped);
    vTaskSuspend(NULL);
}

static HotspotArcadeRuntimeResult runtime_start_dns(void) {
    runtime.dns_ready = xSemaphoreCreateBinary();
    runtime.dns_stopped = xSemaphoreCreateBinary();
    if(!runtime.dns_ready || !runtime.dns_stopped) return HotspotArcadeRuntimeResultNoMemory;

    portENTER_CRITICAL(&runtime_lock);
    runtime.dns_run = true;
    runtime.dns_start_ok = false;
    runtime.dns_socket = -1;
    portEXIT_CRITICAL(&runtime_lock);

    if(xTaskCreateWithCaps(
           runtime_dns_task,
           "HotArcDns",
           HOTSPOT_ARCADE_DNS_STACK,
           NULL,
           4,
           &runtime.dns_task,
           MALLOC_CAP_SPIRAM) != pdPASS) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.dns_run = false;
        portEXIT_CRITICAL(&runtime_lock);
        return HotspotArcadeRuntimeResultNoMemory;
    }

    if(xSemaphoreTake(runtime.dns_ready, pdMS_TO_TICKS(3000)) != pdTRUE) {
        ESP_LOGE(TAG, "DNS responder did not signal ready within 3s");
        return HotspotArcadeRuntimeResultDnsError;
    }

    portENTER_CRITICAL(&runtime_lock);
    const bool started = runtime.dns_start_ok;
    portEXIT_CRITICAL(&runtime_lock);
    return started ? HotspotArcadeRuntimeResultOk : HotspotArcadeRuntimeResultDnsError;
}

static void runtime_stop_dns(void) {
    portENTER_CRITICAL(&runtime_lock);
    runtime.dns_run = false;
    const int socket_fd = runtime.dns_socket;
    TaskHandle_t task = runtime.dns_task;
    portEXIT_CRITICAL(&runtime_lock);

    if(socket_fd >= 0) shutdown(socket_fd, SHUT_RDWR);
    if(task && runtime.dns_stopped) {
        xSemaphoreTake(runtime.dns_stopped, pdMS_TO_TICKS(3000));
        vTaskDeleteWithCaps(task);
    }
    runtime.dns_task = NULL;
    if(runtime.dns_ready) vSemaphoreDelete(runtime.dns_ready);
    if(runtime.dns_stopped) vSemaphoreDelete(runtime.dns_stopped);
    runtime.dns_ready = NULL;
    runtime.dns_stopped = NULL;
}

/* Defined below; the failure paths in runtime_start_wifi() need it. */
static void runtime_stop_wifi(void);

static HotspotArcadeRuntimeResult runtime_start_wifi(void) {
    esp_err_t error = esp_netif_init();
    if(error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(error));
        /* Unwind before bailing. ap_netif is created above, and leaving it
         * allocated cost ~10 KB of INTERNAL DRAM per failed attempt -- so
         * retrying esp_wifi_init after an OOM made the next try strictly
         * worse until a reboot. runtime_stop_wifi() is flag-guarded and
         * safe to call at any stage. */
        runtime_stop_wifi();
        return HotspotArcadeRuntimeResultWifiError;
    }

    error = esp_event_loop_create_default();
    if(error != ESP_OK && error != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event loop: %s", esp_err_to_name(error));
        /* Unwind before bailing. ap_netif is created above, and leaving it
         * allocated cost ~10 KB of INTERNAL DRAM per failed attempt -- so
         * retrying esp_wifi_init after an OOM made the next try strictly
         * worse until a reboot. runtime_stop_wifi() is flag-guarded and
         * safe to call at any stage. */
        runtime_stop_wifi();
        return HotspotArcadeRuntimeResultWifiError;
    }

    runtime.ap_netif = esp_netif_create_default_wifi_ap();
    if(!runtime.ap_netif) return HotspotArcadeRuntimeResultNoMemory;

    esp_netif_dhcps_stop(runtime.ap_netif);
    esp_netif_ip_info_t ip = {0};
    IP4_ADDR(&ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ip.gw, 192, 168, 4, 1);
    IP4_ADDR(&ip.netmask, 255, 255, 255, 0);
    if(esp_netif_set_ip_info(runtime.ap_netif, &ip) != ESP_OK) {
        ESP_LOGE(TAG, "AP bring-up failed at: esp_netif_set_ip_info");
        /* Unwind before bailing. ap_netif is created above, and leaving it
         * allocated cost ~10 KB of INTERNAL DRAM per failed attempt -- so
         * retrying esp_wifi_init after an OOM made the next try strictly
         * worse until a reboot. runtime_stop_wifi() is flag-guarded and
         * safe to call at any stage. */
        runtime_stop_wifi();
        return HotspotArcadeRuntimeResultWifiError;
    }

    wifi_init_config_t wifi_init = WIFI_INIT_CONFIG_DEFAULT();
    wifi_init.static_rx_buf_num = 2;
    wifi_init.dynamic_rx_buf_num = 4;
    wifi_init.dynamic_tx_buf_num = 8;
    error = esp_wifi_init(&wifi_init);
    if(error == ESP_ERR_WIFI_INIT_STATE) {
        /* The WLAN app reserves the WiFi driver during boot and keeps it
         * initialized while the radio is off, so esp_wifi_init() reports the
         * driver is already up. Share it rather than refusing to start.
         * wifi_initialized stays false on purpose: teardown must stop the
         * radio we started without deinitializing a driver we do not own. */
        ESP_LOGI(TAG, "WiFi driver already initialized; sharing it");
    } else if(error != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(error));
        /* Unwind before bailing. ap_netif is created above, and leaving it
         * allocated cost ~10 KB of INTERNAL DRAM per failed attempt -- so
         * retrying esp_wifi_init after an OOM made the next try strictly
         * worse until a reboot. runtime_stop_wifi() is flag-guarded and
         * safe to call at any stage. */
        runtime_stop_wifi();
        /* Report a memory failure AS a memory failure. WifiError maps to
         * "network error" on screen, which sent this investigation chasing the
         * network layer while the real problem was internal DRAM. */
        if(error == ESP_ERR_NO_MEM) return HotspotArcadeRuntimeResultNoMemory;
        return HotspotArcadeRuntimeResultWifiError;
    } else {
        runtime.wifi_initialized = true;
    }

    if(esp_wifi_set_storage(WIFI_STORAGE_RAM) != ESP_OK ||
       esp_wifi_set_mode(WIFI_MODE_AP) != ESP_OK) {
        ESP_LOGE(TAG, "AP bring-up failed at: set_storage/set_mode");
        /* Unwind before bailing. ap_netif is created above, and leaving it
         * allocated cost ~10 KB of INTERNAL DRAM per failed attempt -- so
         * retrying esp_wifi_init after an OOM made the next try strictly
         * worse until a reboot. runtime_stop_wifi() is flag-guarded and
         * safe to call at any stage. */
        runtime_stop_wifi();
        return HotspotArcadeRuntimeResultWifiError;
    }

    wifi_config_t ap = {0};
    const size_t ssid_length = strlen(runtime.ssid);
    memcpy(ap.ap.ssid, runtime.ssid, ssid_length);
    ap.ap.ssid_len = (uint8_t)ssid_length;
    ap.ap.channel = runtime.channel;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = runtime.max_connections;
    ap.ap.beacon_interval = 100;
    if(esp_wifi_set_config(WIFI_IF_AP, &ap) != ESP_OK) {
        ESP_LOGE(TAG, "AP bring-up failed at: esp_wifi_set_config");
        /* Unwind before bailing. ap_netif is created above, and leaving it
         * allocated cost ~10 KB of INTERNAL DRAM per failed attempt -- so
         * retrying esp_wifi_init after an OOM made the next try strictly
         * worse until a reboot. runtime_stop_wifi() is flag-guarded and
         * safe to call at any stage. */
        runtime_stop_wifi();
        return HotspotArcadeRuntimeResultWifiError;
    }

    ESP_LOGI(
        TAG,
        "pre esp_wifi_start: internal DRAM free=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    error = esp_wifi_start();
    if(error != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(error));
        /* Unwind before bailing. ap_netif is created above, and leaving it
         * allocated cost ~10 KB of INTERNAL DRAM per failed attempt -- so
         * retrying esp_wifi_init after an OOM made the next try strictly
         * worse until a reboot. runtime_stop_wifi() is flag-guarded and
         * safe to call at any stage. */
        runtime_stop_wifi();
        /* Report a memory failure AS a memory failure. WifiError maps to
         * "network error" on screen, which sent this investigation chasing the
         * network layer while the real problem was internal DRAM. */
        if(error == ESP_ERR_NO_MEM) return HotspotArcadeRuntimeResultNoMemory;
        return HotspotArcadeRuntimeResultWifiError;
    }
    runtime.wifi_started = true;
    esp_wifi_set_ps(WIFI_PS_NONE);

    esp_netif_dhcps_stop(runtime.ap_netif);
    static char captive_portal_url[] = "http://192.168.4.1/";
    error = esp_netif_dhcps_option(
        runtime.ap_netif,
        ESP_NETIF_OP_SET,
        ESP_NETIF_CAPTIVEPORTAL_URI,
        captive_portal_url,
        sizeof(captive_portal_url) - 1U);
    if(error != ESP_OK) {
        ESP_LOGW(TAG, "DHCP captive URL: %s", esp_err_to_name(error));
    }
    error = esp_netif_dhcps_start(runtime.ap_netif);
    if(error != ESP_OK && error != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
        ESP_LOGE(TAG, "DHCPS start: %s", esp_err_to_name(error));
        /* Unwind before bailing. ap_netif is created above, and leaving it
         * allocated cost ~10 KB of INTERNAL DRAM per failed attempt -- so
         * retrying esp_wifi_init after an OOM made the next try strictly
         * worse until a reboot. runtime_stop_wifi() is flag-guarded and
         * safe to call at any stage. */
        runtime_stop_wifi();
        return HotspotArcadeRuntimeResultWifiError;
    }

    return HotspotArcadeRuntimeResultOk;
}

static void runtime_stop_wifi(void) {
    if(runtime.ap_netif) esp_netif_dhcps_stop(runtime.ap_netif);
    if(runtime.wifi_started) esp_wifi_stop();
    runtime.wifi_started = false;
    if(runtime.wifi_initialized) esp_wifi_deinit();
    runtime.wifi_initialized = false;
    if(runtime.ap_netif) {
        esp_netif_destroy_default_wifi(runtime.ap_netif);
        runtime.ap_netif = NULL;
    }
}

static const char runtime_handoff_html[] =
    "<!doctype html><html><head><meta charset=utf-8>"
    "<meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>Hotspot Arcade</title></head><body>"
    "<h1>Hotspot Arcade</h1><p>Continue in your full browser to play.</p>"
    "<p><a href='http://192.168.4.1/' target='_blank' rel='noopener'>"
    "Open Hotspot Arcade</a></p></body></html>";

static bool runtime_uri_equals(const char* uri, const char* expected) {
    const size_t expected_length = strlen(expected);
    return strncmp(uri, expected, expected_length) == 0 &&
           (uri[expected_length] == '\0' || uri[expected_length] == '?');
}

static bool runtime_is_captive_probe(const char* uri) {
    static const char* const probes[] = {
        "/generate_204",
        "/gen_204",
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/connecttest.txt",
        "/ncsi.txt",
        "/redirect",
        "/canonical.html",
        "/success.txt",
        "/fwlink",
    };

    for(size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        if(runtime_uri_equals(uri, probes[i])) return true;
    }
    return false;
}

static esp_err_t runtime_http_route(httpd_req_t* request) {
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    if(runtime_is_captive_probe(request->uri)) {
        httpd_resp_set_type(request, "text/html; charset=utf-8");
        httpd_resp_set_hdr(request, "Connection", "close");
        return httpd_resp_send(
            request, runtime_handoff_html, sizeof(runtime_handoff_html) - 1U);
    }

    if(!runtime.web_gzip || runtime.web_gzip_length == 0) {
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "web app unavailable");
    }

    httpd_resp_set_type(request, "text/html; charset=utf-8");
    httpd_resp_set_hdr(request, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(request, "Vary", "Accept-Encoding");
    return httpd_resp_send(
        request, (const char*)runtime.web_gzip, (ssize_t)runtime.web_gzip_length);
}

static esp_err_t runtime_http_error_route(httpd_req_t* request, httpd_err_code_t error) {
    (void)error;
    return runtime_http_route(request);
}

#if HOTSPOT_ARCADE_HAS_WEBSOCKET
static RuntimeClient* runtime_add_client(int fd, bool* added) {
    RuntimeClient* client = NULL;
    *added = false;

    portENTER_CRITICAL(&runtime_lock);
    client = runtime_find_client_locked(fd);
    if(!client) {
        for(size_t i = 0; i < HOTSPOT_ARCADE_CLIENT_CAPACITY; ++i) {
            if(!runtime.clients[i].used) {
                client = &runtime.clients[i];
                client->used = true;
                client->fd = fd;
                client->inbound = NULL;
                client->inbound_length = 0;
                client->inbound_text_fragment = false;
                *added = true;
                break;
            }
        }
    }
    portEXIT_CRITICAL(&runtime_lock);
    return client;
}

static bool runtime_client_append(
    RuntimeClient* client,
    const uint8_t* payload,
    size_t length) {
    if(length > runtime.max_ws_message_size - client->inbound_length) return false;
    const size_t new_length = client->inbound_length + length;
    uint8_t* resized = runtime_realloc(client->inbound, new_length + 1U);
    if(!resized) return false;
    client->inbound = resized;
    if(length) memcpy(client->inbound + client->inbound_length, payload, length);
    client->inbound_length = new_length;
    client->inbound[new_length] = '\0';
    return true;
}

static void runtime_client_reset_fragment(RuntimeClient* client) {
    if(client->inbound) free(client->inbound);
    client->inbound = NULL;
    client->inbound_length = 0;
    client->inbound_text_fragment = false;
}

static esp_err_t runtime_ws_handler(httpd_req_t* request) {
    const int fd = httpd_req_to_sockfd(request);
    if(request->method == HTTP_GET) {
        bool added = false;
        RuntimeClient* client = runtime_add_client(fd, &added);
        if(!client) {
            ESP_LOGW(TAG, "WebSocket client table full");
            return ESP_FAIL;
        }
        if(added) runtime_callback_open(fd);
        return ESP_OK;
    }

    httpd_ws_frame_t frame = {0};
    esp_err_t error = httpd_ws_recv_frame(request, &frame, 0);
    if(error != ESP_OK || frame.len > runtime.max_ws_message_size) return ESP_FAIL;

    uint8_t* payload = runtime_alloc(frame.len + 1U);
    if(!payload) return ESP_ERR_NO_MEM;
    frame.payload = payload;
    error = httpd_ws_recv_frame(request, &frame, frame.len);
    if(error != ESP_OK) {
        free(payload);
        return error;
    }
    payload[frame.len] = '\0';

    portENTER_CRITICAL(&runtime_lock);
    RuntimeClient* client = runtime_find_client_locked(fd);
    portEXIT_CRITICAL(&runtime_lock);
    if(!client) {
        free(payload);
        return ESP_FAIL;
    }

    switch(frame.type) {
    case HTTPD_WS_TYPE_TEXT:
        runtime_client_reset_fragment(client);
        if(frame.final) {
            runtime_callback_text(fd, (const char*)payload, frame.len);
        } else {
            client->inbound_text_fragment = true;
            if(!runtime_client_append(client, payload, frame.len)) error = ESP_FAIL;
        }
        break;

    case HTTPD_WS_TYPE_CONTINUE:
        if(!client->inbound_text_fragment || !runtime_client_append(client, payload, frame.len)) {
            error = ESP_FAIL;
            break;
        }
        if(frame.final) {
            runtime_callback_text(
                fd, (const char*)client->inbound, client->inbound_length);
            runtime_client_reset_fragment(client);
        }
        break;

    case HTTPD_WS_TYPE_PING: {
        httpd_ws_frame_t pong = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_PONG,
            .payload = payload,
            .len = frame.len,
        };
        error = httpd_ws_send_frame(request, &pong);
        break;
    }

    case HTTPD_WS_TYPE_CLOSE: {
        httpd_ws_frame_t close_frame = {
            .final = true,
            .fragmented = false,
            .type = HTTPD_WS_TYPE_CLOSE,
            .payload = payload,
            .len = frame.len,
        };
        httpd_ws_send_frame(request, &close_frame);
        error = ESP_FAIL;
        break;
    }

    case HTTPD_WS_TYPE_PONG:
        break;

    default:
        /* Hotspot Arcade is a text protocol; ignore complete binary frames. */
        runtime_client_reset_fragment(client);
        break;
    }

    free(payload);
    return error;
}
#endif

static void runtime_http_close(httpd_handle_t server, int fd) {
    (void)server;
    uint8_t* inbound = NULL;
    bool was_websocket = false;

    portENTER_CRITICAL(&runtime_lock);
    RuntimeClient* client = runtime_find_client_locked(fd);
    if(client) {
        was_websocket = true;
        inbound = client->inbound;
        memset(client, 0, sizeof(*client));
        client->fd = -1;
    }
    portEXIT_CRITICAL(&runtime_lock);

    if(inbound) free(inbound);
    if(was_websocket) runtime_callback_close(fd);
    close(fd);
}

static HotspotArcadeRuntimeResult runtime_start_http(void) {
#if !HOTSPOT_ARCADE_HAS_WEBSOCKET
    return HotspotArcadeRuntimeResultWebSocketDisabled;
#else
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    /* The default ~4 KB overflows: a WebSocket message (JOIN/NAME/game change)
     * runs the engine and the pack reload on THIS task's stack, and the board
     * dies in vPortYieldFromInt with a corrupted backtrace full of 0xa5a5a5a5.
     *
     * 8192 was tried and regressed Start Session to "out of memory" -- this
     * stack comes out of INTERNAL DRAM, the scarcest thing on the board. 6144
     * is the compromise: +2 KB rather than +4 KB. If this still overflows, the
     * answer is to move the engine work off the httpd task, NOT to keep
     * growing the stack. */
    config.stack_size = 6144;
    config.server_port = 80;
    config.max_uri_handlers = 8;
    /* 13 was generous: every open socket costs internal DRAM (lwIP pcb plus
     * an httpd session slot), and internal DRAM is the exact resource this app
     * runs out of. 8 leaves the listener plus ~6 simultaneous players, which is
     * more than the arcade expects, and hands the rest back. */
    config.max_open_sockets = 8;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 5;
    config.send_wait_timeout = 5;
    config.close_fn = runtime_http_close;

    esp_err_t error = httpd_start(&runtime.http, &config);
    if(error != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(error));
        runtime.http = NULL;
        return error == ESP_ERR_NO_MEM ? HotspotArcadeRuntimeResultNoMemory :
                                        HotspotArcadeRuntimeResultHttpError;
    }

    const httpd_uri_t websocket = {
        .uri = "/ws",
        .method = HTTP_GET,
        .handler = runtime_ws_handler,
        .user_ctx = NULL,
        .is_websocket = true,
        .handle_ws_control_frames = true,
        .supported_subprotocol = NULL,
    };
    const httpd_uri_t browser = {
        .uri = "/*",
        .method = HTTP_GET,
        .handler = runtime_http_route,
        .user_ctx = NULL,
        .is_websocket = false,
        .handle_ws_control_frames = false,
        .supported_subprotocol = NULL,
    };

    if(httpd_register_uri_handler(runtime.http, &websocket) != ESP_OK ||
       httpd_register_uri_handler(runtime.http, &browser) != ESP_OK ||
       httpd_register_err_handler(
           runtime.http, HTTPD_404_NOT_FOUND, runtime_http_error_route) != ESP_OK) {
        httpd_stop(runtime.http);
        runtime.http = NULL;
        return HotspotArcadeRuntimeResultHttpError;
    }
    return HotspotArcadeRuntimeResultOk;
#endif
}

#if HOTSPOT_ARCADE_HAS_WEBSOCKET
typedef struct {
    size_t length;
    size_t fd_count;
    int fds[HOTSPOT_ARCADE_CLIENT_CAPACITY];
    uint8_t payload[];
} RuntimeSendWork;

static bool runtime_socket_send_all(
    httpd_handle_t server,
    int fd,
    const uint8_t* data,
    size_t length) {
    size_t sent = 0;
    unsigned timeout_retries = 0;
    while(sent < length) {
        const int result =
            httpd_socket_send(server, fd, (const char*)data + sent, length - sent, 0);
        if(result > 0) {
            sent += (size_t)result;
            timeout_retries = 0;
        } else if(result == HTTPD_SOCK_ERR_TIMEOUT &&
                  timeout_retries++ < HOTSPOT_ARCADE_SEND_TIMEOUT_RETRY) {
            continue;
        } else {
            return false;
        }
    }
    return true;
}

static bool runtime_send_ws_text(httpd_handle_t server, int fd, const uint8_t* data, size_t length) {
    if(httpd_ws_get_fd_info(server, fd) != HTTPD_WS_CLIENT_WEBSOCKET) return false;

    size_t offset = 0;
    bool first = true;
    do {
        size_t fragment_length = length - offset;
        if(fragment_length > HOTSPOT_ARCADE_OUT_FRAGMENT_SIZE) {
            fragment_length = HOTSPOT_ARCADE_OUT_FRAGMENT_SIZE;
        }
        const bool final = offset + fragment_length == length;

        uint8_t header[4];
        size_t header_length = 2;
        header[0] = (final ? 0x80U : 0U) | (first ? 0x01U : 0x00U);
        if(fragment_length <= 125U) {
            header[1] = (uint8_t)fragment_length;
        } else {
            header[1] = 126U;
            header[2] = (uint8_t)(fragment_length >> 8U);
            header[3] = (uint8_t)fragment_length;
            header_length = 4;
        }

        if(!runtime_socket_send_all(server, fd, header, header_length) ||
           (fragment_length > 0 &&
            !runtime_socket_send_all(server, fd, data + offset, fragment_length))) {
            return false;
        }

        offset += fragment_length;
        first = false;
    } while(offset < length || first);
    return true;
}

static void runtime_send_work(void* argument) {
    RuntimeSendWork* work = argument;
    httpd_handle_t server = runtime.http;
    if(server) {
        for(size_t i = 0; i < work->fd_count; ++i) {
            runtime_send_ws_text(server, work->fds[i], work->payload, work->length);
        }
    }

    free(work);
    portENTER_CRITICAL(&runtime_lock);
    if(runtime.tx_work_count) --runtime.tx_work_count;
    portEXIT_CRITICAL(&runtime_lock);
}

static bool runtime_queue_text(
    int client_id,
    bool broadcast,
    const char* text,
    size_t length) {
    if((!text && length != 0) || length > HOTSPOT_ARCADE_RUNTIME_MAX_OUTBOUND_TEXT_SIZE) {
        return false;
    }

    RuntimeSendWork* work = runtime_alloc(sizeof(*work) + length);
    if(!work) return false;
    work->length = length;
    work->fd_count = 0;
    if(length) memcpy(work->payload, text, length);

    httpd_handle_t server = NULL;
    portENTER_CRITICAL(&runtime_lock);
    if(runtime.tx_accepting && runtime.http) {
        if(broadcast) {
            for(size_t i = 0; i < HOTSPOT_ARCADE_CLIENT_CAPACITY; ++i) {
                if(runtime.clients[i].used) {
                    work->fds[work->fd_count++] = runtime.clients[i].fd;
                }
            }
        } else {
            RuntimeClient* client = runtime_find_client_locked(client_id);
            if(client) work->fds[work->fd_count++] = client->fd;
        }

        if(work->fd_count > 0) {
            server = runtime.http;
            ++runtime.tx_enqueuers;
            ++runtime.tx_work_count;
        }
    }
    portEXIT_CRITICAL(&runtime_lock);

    if(!server) {
        free(work);
        return false;
    }

    const bool queued = httpd_queue_work(server, runtime_send_work, work) == ESP_OK;
    portENTER_CRITICAL(&runtime_lock);
    --runtime.tx_enqueuers;
    if(!queued) --runtime.tx_work_count;
    portEXIT_CRITICAL(&runtime_lock);
    if(!queued) free(work);
    return queued;
}
#endif

static void runtime_wait_for_sends(void) {
    for(;;) {
        portENTER_CRITICAL(&runtime_lock);
        const bool complete = runtime.tx_enqueuers == 0 && runtime.tx_work_count == 0;
        portEXIT_CRITICAL(&runtime_lock);
        if(complete) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void runtime_clear_clients(void) {
    for(size_t i = 0; i < HOTSPOT_ARCADE_CLIENT_CAPACITY; ++i) {
        runtime_clear_client(&runtime.clients[i]);
    }
}

static void runtime_teardown(void) {
    portENTER_CRITICAL(&runtime_lock);
    runtime.tx_accepting = false;
    runtime.callbacks_enabled = false;
    portEXIT_CRITICAL(&runtime_lock);

    runtime_wait_for_sends();
    if(runtime.http) {
        httpd_stop(runtime.http);
        runtime.http = NULL;
    }
    runtime_stop_dns();
    runtime_stop_wifi();

    for(;;) {
        portENTER_CRITICAL(&runtime_lock);
        const bool callbacks_complete = runtime.callbacks_inflight == 0;
        portEXIT_CRITICAL(&runtime_lock);
        if(callbacks_complete) break;
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    runtime_clear_clients();
    if(runtime.web_gzip) free(runtime.web_gzip);
    runtime.web_gzip = NULL;
    runtime.web_gzip_length = 0;
}

static HotspotArcadeRuntimeResult runtime_worker_start(void) {
#if !HOTSPOT_ARCADE_HAS_WEBSOCKET
    return HotspotArcadeRuntimeResultWebSocketDisabled;
#else
    HotspotArcadeRuntimeResult result = runtime_load_web_file();
    if(result != HotspotArcadeRuntimeResultOk) return result;

    result = runtime_start_wifi();
    if(result != HotspotArcadeRuntimeResultOk) return result;
    result = runtime_start_dns();
    if(result != HotspotArcadeRuntimeResultOk) return result;

    portENTER_CRITICAL(&runtime_lock);
    runtime.callbacks_enabled = true;
    runtime.tx_accepting = true;
    portEXIT_CRITICAL(&runtime_lock);

    result = runtime_start_http();
    return result;
#endif
}

static void runtime_worker(void* context) {
    (void)context;
    HotspotArcadeRuntimeResult result = runtime_worker_start();
    if(result != HotspotArcadeRuntimeResultOk) {
        runtime_teardown();
        portENTER_CRITICAL(&runtime_lock);
        runtime.start_result = result;
        runtime.state = RuntimeStateQuiescent;
        portEXIT_CRITICAL(&runtime_lock);
        xSemaphoreGive(runtime.start_done);
        xSemaphoreGive(runtime.stop_done);
        for(;;) vTaskSuspend(NULL);
    }

    portENTER_CRITICAL(&runtime_lock);
    runtime.start_result = HotspotArcadeRuntimeResultOk;
    runtime.state = RuntimeStateRunning;
    portEXIT_CRITICAL(&runtime_lock);
    ESP_LOGI(
        TAG,
        "started SSID='%s' channel=%u web=%u bytes",
        runtime.ssid,
        runtime.channel,
        (unsigned)runtime.web_gzip_length);
    xSemaphoreGive(runtime.start_done);

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    runtime_teardown();
    portENTER_CRITICAL(&runtime_lock);
    runtime.state = RuntimeStateQuiescent;
    portEXIT_CRITICAL(&runtime_lock);
    ESP_LOGI(TAG, "stopped");
    xSemaphoreGive(runtime.stop_done);
    for(;;) vTaskSuspend(NULL);
}

static bool runtime_validate_config(
    const HotspotArcadeRuntimeConfig* config,
    size_t* ssid_length,
    size_t* path_length) {
    if(!config || !config->ssid || !config->web_gzip_path) return false;
    *ssid_length = strnlen(config->ssid, 33U);
    *path_length = strnlen(config->web_gzip_path, HOTSPOT_ARCADE_WEB_PATH_MAX + 1U);
    if(*ssid_length == 0 || *ssid_length > 32U || *path_length == 0 ||
       *path_length > HOTSPOT_ARCADE_WEB_PATH_MAX) {
        return false;
    }
    if(config->channel > 13U || config->max_connections > HOTSPOT_ARCADE_CLIENT_CAPACITY) {
        return false;
    }
    return true;
}

HotspotArcadeRuntimeResult
    hotspot_arcade_runtime_start(const HotspotArcadeRuntimeConfig* config) {
    size_t ssid_length = 0;
    size_t path_length = 0;
    if(!runtime_validate_config(config, &ssid_length, &path_length)) {
        return HotspotArcadeRuntimeResultInvalidArgument;
    }

    SemaphoreHandle_t start_done = xSemaphoreCreateBinary();
    SemaphoreHandle_t stop_done = xSemaphoreCreateBinary();
    if(!start_done || !stop_done) {
        if(start_done) vSemaphoreDelete(start_done);
        if(stop_done) vSemaphoreDelete(stop_done);
        return HotspotArcadeRuntimeResultNoMemory;
    }

    portENTER_CRITICAL(&runtime_lock);
    if(runtime.state != RuntimeStateStopped) {
        portEXIT_CRITICAL(&runtime_lock);
        vSemaphoreDelete(start_done);
        vSemaphoreDelete(stop_done);
        return HotspotArcadeRuntimeResultAlreadyRunning;
    }

    memcpy(runtime.ssid, config->ssid, ssid_length);
    runtime.ssid[ssid_length] = '\0';
    memcpy(runtime.web_gzip_path, config->web_gzip_path, path_length);
    runtime.web_gzip_path[path_length] = '\0';
    runtime.callback_context = config->callback_context;
    runtime.on_ws_open = config->on_ws_open;
    runtime.on_ws_close = config->on_ws_close;
    runtime.on_ws_text = config->on_ws_text;
    runtime.max_ws_message_size = config->max_ws_message_size ?
                                      config->max_ws_message_size :
                                      HOTSPOT_ARCADE_RUNTIME_DEFAULT_MAX_WS_MESSAGE_SIZE;
    runtime.channel = config->channel ? config->channel : HOTSPOT_ARCADE_RUNTIME_DEFAULT_CHANNEL;
    runtime.max_connections = config->max_connections ?
                                  config->max_connections :
                                  HOTSPOT_ARCADE_RUNTIME_DEFAULT_MAX_CONNECTIONS;
    runtime.start_done = start_done;
    runtime.stop_done = stop_done;
    runtime.start_result = HotspotArcadeRuntimeResultInternalError;
    runtime.callbacks_enabled = false;
    runtime.callbacks_inflight = 0;
    runtime.tx_accepting = false;
    runtime.tx_enqueuers = 0;
    runtime.tx_work_count = 0;
    runtime.dns_socket = -1;
    runtime.state = RuntimeStateStarting;
    portEXIT_CRITICAL(&runtime_lock);

    TaskHandle_t worker = NULL;
    if(xTaskCreate(
           runtime_worker,
           "HotArcWorker",
           HOTSPOT_ARCADE_WORKER_STACK,
           NULL,
           5,
           &worker) != pdPASS) {
        portENTER_CRITICAL(&runtime_lock);
        runtime.state = RuntimeStateStopped;
        runtime.start_done = NULL;
        runtime.stop_done = NULL;
        portEXIT_CRITICAL(&runtime_lock);
        vSemaphoreDelete(start_done);
        vSemaphoreDelete(stop_done);
        return HotspotArcadeRuntimeResultNoMemory;
    }

    portENTER_CRITICAL(&runtime_lock);
    runtime.worker_task = worker;
    portEXIT_CRITICAL(&runtime_lock);
    xSemaphoreTake(start_done, portMAX_DELAY);

    portENTER_CRITICAL(&runtime_lock);
    const HotspotArcadeRuntimeResult result = runtime.start_result;
    portEXIT_CRITICAL(&runtime_lock);
    vSemaphoreDelete(start_done);
    portENTER_CRITICAL(&runtime_lock);
    runtime.start_done = NULL;
    portEXIT_CRITICAL(&runtime_lock);

    if(result != HotspotArcadeRuntimeResultOk) {
        xSemaphoreTake(stop_done, portMAX_DELAY);
        vTaskDelete(worker);
        portENTER_CRITICAL(&runtime_lock);
        runtime.worker_task = NULL;
        runtime.stop_done = NULL;
        runtime.callback_context = NULL;
        runtime.on_ws_open = NULL;
        runtime.on_ws_close = NULL;
        runtime.on_ws_text = NULL;
        runtime.state = RuntimeStateStopped;
        portEXIT_CRITICAL(&runtime_lock);
        vSemaphoreDelete(stop_done);
    }
    return result;
}

HotspotArcadeRuntimeResult hotspot_arcade_runtime_stop(void) {
    portENTER_CRITICAL(&runtime_lock);
    if(runtime.state == RuntimeStateStopped) {
        portEXIT_CRITICAL(&runtime_lock);
        return HotspotArcadeRuntimeResultNotRunning;
    }
    if(runtime.state != RuntimeStateRunning || !runtime.worker_task || !runtime.stop_done) {
        portEXIT_CRITICAL(&runtime_lock);
        return HotspotArcadeRuntimeResultInternalError;
    }
    runtime.state = RuntimeStateStopping;
    TaskHandle_t worker = runtime.worker_task;
    SemaphoreHandle_t stop_done = runtime.stop_done;
    portEXIT_CRITICAL(&runtime_lock);

    xTaskNotifyGive(worker);
    xSemaphoreTake(stop_done, portMAX_DELAY);
    vTaskDelete(worker);

    portENTER_CRITICAL(&runtime_lock);
    runtime.worker_task = NULL;
    runtime.stop_done = NULL;
    runtime.callback_context = NULL;
    runtime.on_ws_open = NULL;
    runtime.on_ws_close = NULL;
    runtime.on_ws_text = NULL;
    runtime.state = RuntimeStateStopped;
    portEXIT_CRITICAL(&runtime_lock);
    vSemaphoreDelete(stop_done);
    return HotspotArcadeRuntimeResultOk;
}

bool hotspot_arcade_runtime_is_running(void) {
    portENTER_CRITICAL(&runtime_lock);
    const bool running = runtime.state == RuntimeStateRunning;
    portEXIT_CRITICAL(&runtime_lock);
    return running;
}

bool hotspot_arcade_runtime_send_text(
    HotspotArcadeRuntimeClientId client_id,
    const char* text,
    size_t length) {
#if HOTSPOT_ARCADE_HAS_WEBSOCKET
    return runtime_queue_text(client_id, false, text, length);
#else
    (void)client_id;
    (void)text;
    (void)length;
    return false;
#endif
}

bool hotspot_arcade_runtime_broadcast_text(const char* text, size_t length) {
#if HOTSPOT_ARCADE_HAS_WEBSOCKET
    return runtime_queue_text(-1, true, text, length);
#else
    (void)text;
    (void)length;
    return false;
#endif
}

size_t hotspot_arcade_runtime_connected_count(void) {
    portENTER_CRITICAL(&runtime_lock);
    const size_t count =
        runtime.state == RuntimeStateRunning ? runtime_connected_count_locked() : 0;
    portEXIT_CRITICAL(&runtime_lock);
    return count;
}

const char* hotspot_arcade_runtime_result_to_string(HotspotArcadeRuntimeResult result) {
    switch(result) {
    case HotspotArcadeRuntimeResultOk:
        return "ok";
    case HotspotArcadeRuntimeResultInvalidArgument:
        return "invalid argument";
    case HotspotArcadeRuntimeResultAlreadyRunning:
        return "already running";
    case HotspotArcadeRuntimeResultNotRunning:
        return "not running";
    case HotspotArcadeRuntimeResultNoMemory:
        return "out of memory";
    case HotspotArcadeRuntimeResultStorageError:
        return "web file read failed";
    case HotspotArcadeRuntimeResultWifiError:
        return "WiFi start failed";
    case HotspotArcadeRuntimeResultHttpError:
        return "HTTP server start failed";
    case HotspotArcadeRuntimeResultDnsError:
        return "DNS server start failed";
    case HotspotArcadeRuntimeResultWebSocketDisabled:
        return "ESP-IDF WebSocket support disabled";
    case HotspotArcadeRuntimeResultInternalError:
    default:
        return "internal error";
    }
}
