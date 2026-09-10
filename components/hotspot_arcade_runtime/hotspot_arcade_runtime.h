/**
 * @file hotspot_arcade_runtime.h
 *
 * ESP32-S3 transport used by the Hotspot Arcade FAP.
 *
 * The runtime is a process-wide singleton because ESP-IDF's WiFi driver,
 * default network interfaces, and HTTP server are process-wide resources.
 * Start and stop are synchronous and serialized. WebSocket callbacks run on
 * ESP-IDF tasks, not on the caller's FuriThread, and must return promptly.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOTSPOT_ARCADE_RUNTIME_DEFAULT_CHANNEL             1U
#define HOTSPOT_ARCADE_RUNTIME_DEFAULT_MAX_CONNECTIONS     8U
#define HOTSPOT_ARCADE_RUNTIME_DEFAULT_MAX_WS_MESSAGE_SIZE 4096U
#define HOTSPOT_ARCADE_RUNTIME_MAX_OUTBOUND_TEXT_SIZE       (32U * 1024U)

/** A WebSocket client identifier. Valid only until its close callback. */
typedef int HotspotArcadeRuntimeClientId;

typedef enum {
    HotspotArcadeRuntimeResultOk = 0,
    HotspotArcadeRuntimeResultInvalidArgument,
    HotspotArcadeRuntimeResultAlreadyRunning,
    HotspotArcadeRuntimeResultNotRunning,
    HotspotArcadeRuntimeResultNoMemory,
    HotspotArcadeRuntimeResultStorageError,
    HotspotArcadeRuntimeResultWifiError,
    HotspotArcadeRuntimeResultHttpError,
    HotspotArcadeRuntimeResultDnsError,
    HotspotArcadeRuntimeResultWebSocketDisabled,
    HotspotArcadeRuntimeResultInternalError,
} HotspotArcadeRuntimeResult;

typedef void (*HotspotArcadeRuntimeWsOpenCallback)(
    void* context,
    HotspotArcadeRuntimeClientId client_id);

typedef void (*HotspotArcadeRuntimeWsCloseCallback)(
    void* context,
    HotspotArcadeRuntimeClientId client_id);

/**
 * Called for one complete UTF-8 WebSocket text frame.
 *
 * The text is NUL-terminated for convenience, but length is authoritative and
 * the pointer is valid only for the duration of the callback.
 */
typedef void (*HotspotArcadeRuntimeWsTextCallback)(
    void* context,
    HotspotArcadeRuntimeClientId client_id,
    const char* text,
    size_t length);

typedef struct {
    /** Open SoftAP SSID, 1..32 bytes. Copied by start(). */
    const char* ssid;

    /**
     * Storage path of the pre-compressed single-page web client, for example
     * /ext/apps_assets/hotspot_arcade/web/index.html.gz. The file is loaded
     * into PSRAM during start(), so HTTP callbacks never block on SD I/O.
     */
    const char* web_gzip_path;

    /** Opaque pointer passed to all callbacks. */
    void* callback_context;

    HotspotArcadeRuntimeWsOpenCallback on_ws_open;
    HotspotArcadeRuntimeWsCloseCallback on_ws_close;
    HotspotArcadeRuntimeWsTextCallback on_ws_text;

    /** 0 selects HOTSPOT_ARCADE_RUNTIME_DEFAULT_MAX_WS_MESSAGE_SIZE. */
    uint16_t max_ws_message_size;

    /** 0 selects HOTSPOT_ARCADE_RUNTIME_DEFAULT_CHANNEL; valid range 1..13. */
    uint8_t channel;

    /**
     * 0 selects HOTSPOT_ARCADE_RUNTIME_DEFAULT_MAX_CONNECTIONS. The runtime
     * currently accepts 1..8 stations.
     */
    uint8_t max_connections;
} HotspotArcadeRuntimeConfig;

/**
 * Start the AP, wildcard DNS server, HTTP server, and /ws endpoint.
 *
 * This function copies the config, loads web_gzip_path from storage, stops BLE
 * when necessary, and blocks until the transport is ready or fully rolled
 * back. Do not call start() or stop() from a runtime callback.
 */
HotspotArcadeRuntimeResult
    hotspot_arcade_runtime_start(const HotspotArcadeRuntimeConfig* config);

/**
 * Stop the transport and restore BLE if start() stopped it.
 *
 * When this function returns, all runtime tasks and queued sends are quiescent
 * and no callback from the stopped session can run again.
 */
HotspotArcadeRuntimeResult hotspot_arcade_runtime_stop(void);

bool hotspot_arcade_runtime_is_running(void);

/**
 * Queue a copied UTF-8 text frame for one client.
 *
 * A true return means the frame was accepted by the bounded transport queue;
 * delivery can still fail if the peer disconnects before it is sent.
 */
bool hotspot_arcade_runtime_send_text(
    HotspotArcadeRuntimeClientId client_id,
    const char* text,
    size_t length);

/** Queue a copied UTF-8 text frame for every currently connected client. */
bool hotspot_arcade_runtime_broadcast_text(const char* text, size_t length);

size_t hotspot_arcade_runtime_connected_count(void);

const char* hotspot_arcade_runtime_result_to_string(HotspotArcadeRuntimeResult result);

#ifdef __cplusplus
}
#endif
