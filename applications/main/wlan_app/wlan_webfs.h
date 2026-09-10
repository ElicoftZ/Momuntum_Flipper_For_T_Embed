#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Web-Filesystem: HTTP file server for the SD (/ext), servable either as a
 * dedicated WPA2 SoftAP or over the current wlan_hal STA connection. The HTML UI
 * is loaded from /ext/webfs/index.html; the REST API talks to the Storage record.
 *
 * esp_wifi_* / httpd_start need a real task context, so bring-up is dispatched
 * onto the wlan_hal worker (wlan_hal_run_in_worker). AP mode takes over the radio
 * (stops STA + BLE, restored on stop); STA mode reuses the existing connection. */

#define WLAN_WEBFS_SSID_MAX     32
#define WLAN_WEBFS_PW_MAX       63
#define WLAN_WEBFS_DEFAULT_SSID "Flipper32"
#define WLAN_WEBFS_DEFAULT_PW   "esp32ftw"

/* Dedicated-AP SSID/password persistence (/ext/webfs/config.txt). Missing file
 * or field falls back to the default; load always returns true. */
bool wlan_webfs_config_load(char* ssid_out, char* pw_out);
bool wlan_webfs_config_save(const char* ssid, const char* pw);

/* Start a dedicated WPA2 SoftAP (password < 8 chars = open) + file server.
 * Blocks until up or failed. */
bool wlan_webfs_start_ap(const char* ssid, const char* password);

/* Start the file server on the current wlan_hal STA connection.
 * Requires wlan_hal_is_connected(). Blocks until up or failed. */
bool wlan_webfs_start_sta(void);

void wlan_webfs_stop(void);
bool wlan_webfs_is_running(void);
bool wlan_webfs_is_ap(void);

/* Writes the server IP into out (len >= 16). */
bool wlan_webfs_get_ip(char* out, size_t len);

/* Associated clients (AP mode only; 0 in STA mode). */
uint8_t wlan_webfs_get_client_count(void);
