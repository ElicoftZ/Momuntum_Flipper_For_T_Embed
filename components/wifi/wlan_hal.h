#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <esp_wifi.h>

/** Start WiFi in STA mode. Retry a failed start with the BLE stack suspended
 *  to reclaim internal RAM. Restore BLE after temporary WiFi use. Idempotent. */
bool wlan_hal_start(void);

/** Stop WiFi. Persistent WiFi keeps its reservation; a foreground-only session
 *  fully releases it. */
void wlan_hal_stop(void);

/** Finish a foreground WiFi-app session without reconnecting early.
 *
 * The desktop resumes a persistently enabled radio only after the app thread
 * and all of its large view buffers have been released. */
void wlan_hal_finish_foreground_session(void);

/** Steuert, ob wlan_hal_stop() den BT-Stack wiederherstellt. wlan_hal_start()
 *  setzt dieses Flag automatisch auf true, wenn es BT beim Start abgeschaltet
 *  hat (transiente WiFi-Nutzung → BT kommt beim Stop zurück). Der WiFi-Service
 *  löscht es (false), sobald WiFi global/„sticky" wird (Lock-Menü „Enable WiFi"
 *  oder erfolgreicher STA-Connect), damit BT dann bewusst aus bleibt. */
void wlan_hal_set_bt_restore(bool restore);

bool wlan_hal_is_started(void);

/** User-facing WiFi switch used by the desktop lock menu. It is independent of
 *  the Bluetooth switch. While enabled, normal WLAN-app teardown returns to
 *  background STA and reconnects the last successful SSID. Association itself
 *  is asynchronous. */
bool wlan_hal_set_user_enabled(bool enabled);
bool wlan_hal_is_user_enabled(void);

/** Restore the persisted WiFi switch on normal boot. If the switch is off,
 *  WiFi remains off. Every successful connection performs one SNTP update,
 *  stores it in the ESP32 system RTC, then shuts SNTP down. */
void wlan_hal_start_boot_time_sync(void);
bool wlan_hal_is_boot_time_sync_active(void);

/** Reserve the small WiFi driver/worker allocation before the GUI fragments
 *  internal RAM, then restore persistent WiFi if its user switch is enabled.
 *  With the switch off the radio remains stopped, but later scans can start
 *  without repeating the large contiguous allocation. Safe to call repeatedly
 *  and while BLE is active. */
void wlan_hal_prepare_radio_memory(void);

/** Skip wlan_hal_prepare_radio_memory()'s WiFi auto-reconnect exactly once,
 *  on the very next boot, without changing the user's persistent enable
 *  setting. Call this right before rebooting into freshly-flashed firmware
 *  (after a successful OTA install) so Bluetooth features have full memory
 *  headroom immediately post-update; WiFi resumes normally starting the
 *  boot after that. */
void wlan_hal_hold_wifi_after_reboot(void);

/** True from the moment wlan_hal_prepare_radio_memory() consumes the
 *  post-update hold until WiFi actually starts for any reason, for the rest
 *  of this boot only. Lets status displays show "off" for a persistently
 *  enabled setting that is deliberately not running yet. */
bool wlan_hal_is_held_after_update(void);

/** For an operation that needs WiFi's internal DRAM freed while it runs (a
 *  memory-hungry write, or a BLE takeover). Fully stops and deinits WiFi and
 *  releases its worker stack if it was running - wlan_hal_suspend_user_radio()
 *  alone does not go this far. Pair with wlan_hal_resume_user_radio() when
 *  done; safe to call even if WiFi was already off. Deliberately does not
 *  touch BLE state - the caller owns that. */
bool wlan_hal_yield_for_memory(void);

/** Ensure the small hardware-AES DMA reserve is held before the WLAN app
 * allocates its GUI objects. */
bool wlan_hal_prepare_foreground_session(void);

/** Force one fresh time update. If STA is disconnected, asynchronously use
 *  the last saved network and restore the previous radio state afterward.
 *  Returns false only when no saved network exists or the task cannot start. */
bool wlan_hal_start_manual_time_sync(void);

/** Temporarily release/reacquire the WiFi driver without changing the user's
 *  enabled switch. Desktop uses this around apps which own the WiFi hardware. */
void wlan_hal_suspend_user_radio(void);
bool wlan_hal_resume_user_radio(void);

/** Fully stop/deinit STA for a WiFi mode change while keeping the user's WiFi
 *  switch intact. */
void wlan_hal_stop_for_reconfigure(void);

/** Stellt nur den WLAN-Worker-Task + Command-Queue sicher, ohne den WiFi-
 *  Stack zu initialisieren. Nötig, bevor wlan_hal_run_in_worker()/Evil-
 *  Portal aufgerufen werden, falls vorher kein wlan_hal_start() lief.
 *  Idempotent. */
bool wlan_hal_ensure_worker(void);

/** Synchronous active scan on all channels. Allocates *out_records with
 * malloc; caller frees. Returns false on driver/allocation failure, true for
 * a successful scan (including zero APs). Empty/error results are NULL/0. */
bool wlan_hal_scan(wifi_ap_record_t** out_records, uint16_t* out_count, uint16_t max_count);

/** Versuche Verbindung zu SSID. Nicht-blockierend — polle wlan_hal_is_connected()
 *  oder warte auf den IP_EVENT. Auto-Reconnect ist aktiv bis wlan_hal_disconnect(). */
bool wlan_hal_connect(const char* ssid, const char* password, const uint8_t* bssid, uint8_t channel);

/** Last esp-idf disconnect reason, or zero before/after a successful attempt. */
uint8_t wlan_hal_get_last_disconnect_reason(void);

void wlan_hal_disconnect(void);

/** Queue a station disconnect without waiting for the WiFi driver worker.
 * Intended for GUI timeout/error paths, which must never block input/rendering.
 */
bool wlan_hal_disconnect_async(void);

bool wlan_hal_is_connected(void);

/** Re-run automatic IP-based timezone detection now. The call is queued on
 * the WiFi worker and does nothing while disconnected or in manual mode. */
void wlan_hal_request_timezone_refresh(void);

/** Füllt out mit dem AP, mit dem die STA aktuell verbunden ist (SSID, BSSID,
 *  Channel, Authmode, RSSI via esp_wifi_sta_get_ap_info). Liefert false, wenn
 *  nicht verbunden oder der Stack nicht läuft. Nützlich, um den UI-Zustand einer
 *  neu gestarteten App aus einer bereits bestehenden (globalen) Verbindung zu
 *  rekonstruieren. */
bool wlan_hal_get_connected_ap(wifi_ap_record_t* out);

bool wlan_hal_last_fail_is_auth(void);

/** Eigene IP nach erfolgreichem Connect (Network-Byte-Order). 0 wenn keine. */
uint32_t wlan_hal_get_own_ip(void);

/** Netmask der STA-Verbindung (Network-Byte-Order). 0 wenn nicht verbunden. */
uint32_t wlan_hal_get_netmask(void);

/** Eigene MAC-Adresse der STA-Schnittstelle. Liefert false wenn WiFi nicht
 *  läuft. out muss 6 Bytes haben. */
bool wlan_hal_get_own_mac(uint8_t out[6]);

/** Gateway-IP (Network-Byte-Order). 0 wenn nicht verbunden. */
uint32_t wlan_hal_get_gw_ip(void);

/** Sende einen rohen Ethernet-Frame (Eth-Header + Payload) über die STA-IF
 *  via esp_wifi_internal_tx(). Für ARP-Injection durch wlan_netcut. */
bool wlan_hal_send_eth_raw(const uint8_t* data, uint16_t len);

/** Setzt den primären Kanal (1..14). */
void wlan_hal_set_channel(uint8_t channel);

/** Aktiviert/deaktiviert Promiscuous-RX. cb wird beim Aktivieren gesetzt
 *  und kann NULL sein (z.B. zum bloßen Channel-Setzen). */
void wlan_hal_set_promiscuous(bool enable, wifi_promiscuous_cb_t cb);

/** Sende einen rohen 802.11-Frame (für Deauth/Disassoc) via
 *  esp_wifi_80211_tx(). Frame-Länge bis 64 Bytes. */
bool wlan_hal_send_raw(const uint8_t* data, uint16_t len);

/** Direkter roher 802.11-TX auf dem STA-Interface mit Retry bei vollem TX-Ring
 *  (ESP_ERR_NO_MEM, bis zu 3 Versuche mit je 1 Tick Pause). Anders als
 *  wlan_hal_send_raw() läuft dies NICHT über den wlan-Worker, sondern direkt im
 *  aufrufenden Task — für High-Rate-Deauth-Bursts, die den Queue-Roundtrip nicht
 *  vertragen. Nur aus einem echten FreeRTOS-Task mit aktivem Promiscuous
 *  aufrufen. true = Frame ging in den TX-Ring. */
bool wlan_hal_raw_tx_retry(const uint8_t* data, uint16_t len);

/** Generischer Worker-Hook: ruft fn(arg) im wlan-Worker-Task auf und blockt
 *  bis er fertig ist. Nötig für Code, der esp_wifi_*-APIs vom Worker aus
 *  treiben muss (z.B. Evil Portal: SoftAP-Init, Verify-Connect). */
typedef void (*WlanHalWorkerFn)(void* arg);
bool wlan_hal_run_in_worker(WlanHalWorkerFn fn, void* arg);

/** Beacon-Spam-Modi für wlan_hal_beacon_spam_start(). */
typedef enum {
    WlanHalBeaconModeFunny,
    WlanHalBeaconModeRickroll,
    WlanHalBeaconModeRandom,
    WlanHalBeaconModeCustom,
} WlanHalBeaconMode;

/** Startet einen Hintergrund-Task, der Beacon-Frames mit zufälligen MACs
 *  und SSIDs aus der gewählten Quelle sendet. Channel rotiert alle 5
 *  Pakete (1..11). base_ssid wird nur im Custom-Mode verwendet (Suffix
 *  Counter). Kein Effekt wenn bereits running. */
void wlan_hal_beacon_spam_start(WlanHalBeaconMode mode, const char* base_ssid);

/** Stoppt den Beacon-Task synchron. Idempotent. */
void wlan_hal_beacon_spam_stop(void);

bool wlan_hal_beacon_spam_is_running(void);

uint32_t wlan_hal_beacon_spam_get_frame_count(void);

/** Startet einen Hintergrund-Task, der Probe-Request-Frames mit zufälligen
 *  Quell-MACs und zufälligen SSID-Namen sendet (wie ein Gerät, das nach
 *  vielen verschiedenen bekannten Netzen sucht). Channel rotiert alle 5
 *  Paketen (1..11), wie beim Beacon-Spam. Kein Effekt wenn bereits running. */
void wlan_hal_probe_flood_start(void);

/** Stoppt den Probe-Flood-Task synchron. Idempotent. */
void wlan_hal_probe_flood_stop(void);

bool wlan_hal_probe_flood_is_running(void);

uint32_t wlan_hal_probe_flood_get_frame_count(void);

// ---------------------------------------------------------------------------
// Evil Portal: SoftAP + DNS-Hijack + HTTP-Captive-Server.
// ---------------------------------------------------------------------------

typedef void (*WlanHalEvilPortalCredCb)(const char* user, const char* pwd, void* ctx);
typedef void (*WlanHalEvilPortalValidCb)(const char* ssid, const char* pwd, void* ctx);
typedef void (*WlanHalEvilPortalBusyCb)(bool busy, const char* msg, void* ctx);

typedef struct {
    const char* ssid;
    uint8_t channel;
    bool verify_creds;            // Router-Mode: gegen echte APs verifizieren
    bool karma;                   // Karma: Probe-Requests sniffen + AP-SSID
                                  // dynamisch auf die meistgesuchte SSID stellen
    const char* html;
    size_t html_len;
    const char* router_ssid_options; // optional, ersetzt %SSID_OPTIONS%
    WlanHalEvilPortalCredCb cred_cb;
    void* cred_cb_ctx;
    WlanHalEvilPortalValidCb valid_cb;
    void* valid_cb_ctx;
    WlanHalEvilPortalBusyCb busy_cb;
    void* busy_cb_ctx;
    // If true, after step=2 cred capture the portal serves a "Connecting..."
    // page that meta-refreshes to google.com after ~7s (gives bridge time to
    // come up + NAPT to activate). If false, serves the "Couldn't sign you in"
    // page as before.
    bool bridge_redirect;
} WlanHalEvilPortalConfig;

bool wlan_hal_evil_portal_start(const WlanHalEvilPortalConfig* cfg);
void wlan_hal_evil_portal_stop(void);
bool wlan_hal_evil_portal_is_running(void);
bool wlan_hal_evil_portal_verify_creds(const char* ssid, const char* pwd);
void wlan_hal_evil_portal_pause(void);
void wlan_hal_evil_portal_resume(void);
bool wlan_hal_evil_portal_is_paused(void);
uint32_t wlan_hal_evil_portal_get_cred_count(void);
uint16_t wlan_hal_evil_portal_get_client_count(void);

/** Karma: Anzahl bisher geernteter (eindeutiger) Probe-SSIDs. */
uint16_t wlan_hal_evil_portal_karma_get_ssid_count(void);

/** Karma: aktuell vom SoftAP gespoofte SSID nach out kopieren.
 *  Liefert false wenn Karma inaktiv. */
bool wlan_hal_evil_portal_karma_get_current(char* out, size_t out_size);

// DNS forward mode: when set with non-zero upstream_ip, the evil portal's DNS
// task stops hijacking and instead forwards queries to upstream_ip:53 (the
// upstream DNS server learned from STA DHCP). Set to 0 to re-enable
// hijack. Called by the bridge when it goes active so clients can resolve
// real hostnames and browse the internet via NAT. upstream_ip is in network
// byte order (matches esp_netif_ip_info_t / lwIP ip4_addr_t layout).
void wlan_hal_evil_portal_set_dns_upstream(uint32_t upstream_ip_be);
