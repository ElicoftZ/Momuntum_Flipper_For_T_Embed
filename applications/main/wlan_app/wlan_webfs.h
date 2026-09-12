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

/* Read-only demo AP with a captive-portal popup (DNS hijack + the standard
 * OS connectivity-check endpoints redirected), like joining hotel/airport
 * WiFi: connecting pops the page open automatically. Serves exactly one
 * file, no file APIs. ssid/page_path may be NULL/empty to fall back to the
 * defaults below. */
#define WLAN_SAFE_PORTAL_DEFAULT_SSID "TEmbed-Demo"
#define WLAN_SAFE_PORTAL_DEFAULT_PAGE "/ext/safe_portal/index.html"
#define WLAN_SAFE_PORTAL_PAGE_MAX     127
bool wlan_webfs_start_safe(const char* ssid, const char* page_path);

/* Safe-portal SSID/page, persisted separately from the dedicated-AP
 * SSID/password above so picking a prank name/page never touches the real
 * Web-FS config. */
bool wlan_webfs_safe_ssid_load(char* ssid_out);
bool wlan_webfs_safe_ssid_save(const char* ssid);
bool wlan_webfs_safe_page_load(char* path_out);
bool wlan_webfs_safe_page_save(const char* path);

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
