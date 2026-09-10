#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Firmware-Self-Update: lädt furi_esp32.bin von der Release-URL auf die SD und
 *  flasht sie per esp_ota_write in die inaktive OTA-Partition (danach Reboot).
 *
 *  Braucht ein Dual-OTA-Partitionslayout (ota_0/ota_1/otadata, siehe
 *  partitions_ota_16mb.csv). Auf einem Single-App-Build liefert
 *  esp_ota_get_next_update_partition() NULL → Flash-Phase meldet Error.
 *
 *  Jede Phase läuft in einem dedizierten xTaskCreate-Task (esp_http_client nutzt
 *  lwIP-Sockets, die nicht aus FuriThreads getrieben werden dürfen; der
 *  esp_ota_write-Task macht Flash-Writes und braucht daher einen internen
 *  DRAM-Stack — kein PSRAM-Stack, sonst DoubleException). Fortschritt wird über
 *  volatile Felder publiziert und von der Scene im Tick gepollt. */

typedef enum {
    FwUpdateIdle = 0,
    FwUpdateChecking,     // lädt Remote-version.txt und vergleicht
    FwUpdateUpToDate,     // lokale == remote
    FwUpdateAvailable,    // Update vorhanden, remote_version gesetzt
    FwUpdateDownloading,  // lädt furi_esp32.bin auf die SD
    FwUpdateDownloaded,   // .bin liegt auf der SD, bereit zum Flashen
    FwUpdateFlashing,     // esp_ota_write läuft
    FwUpdateDone,         // geflasht, bereit für Reboot
    FwUpdateError,
} FwUpdatePhase;

typedef struct WlanFwUpdate WlanFwUpdate;

WlanFwUpdate* wlan_fw_update_alloc(void);

/** Schreibt die einkompilierte Version (FURI_ESP32_VERSION aus
 *  <toolbox/fw_version.h>) in den Marker /ext/.fw_version, falls dieser fehlt
 *  oder abweicht. Der Marker ist damit immer die FW-Version, die gerade laeuft
 *  — lesbar von aussen (WebFS/qFlipper) und Fallback fuer den Versions-Check.
 *  No-op wenn er schon stimmt. Nur Storage-IO, aus jedem Thread aufrufbar. */
void wlan_fw_update_sync_marker(void);

void wlan_fw_update_free(WlanFwUpdate* u);

/** Startet den Versions-Check (Remote-version.txt vs. FURI_ESP32_VERSION). */
void wlan_fw_update_check_start(WlanFwUpdate* u);

/** Startet den Download der furi_esp32.bin nach /ext/update/. */
void wlan_fw_update_download_start(WlanFwUpdate* u);

/** Startet das Flashen der heruntergeladenen .bin in die inaktive Partition.
 *  Nach Erfolg (Phase FwUpdateDone) muss der Aufrufer esp_restart() aufrufen. */
void wlan_fw_update_flash_start(WlanFwUpdate* u);

/** Fordert Abbruch an und blockt (bis ~10 s) bis der Worker beendet ist.
 *  Während der Flash-Phase wirkt der Abbruch nicht sofort — ein halb
 *  geschriebenes inaktives Image ist unkritisch. */
void wlan_fw_update_cancel(WlanFwUpdate* u);

FwUpdatePhase wlan_fw_update_get_phase(const WlanFwUpdate* u);
uint8_t wlan_fw_update_get_percent(const WlanFwUpdate* u);
const char* wlan_fw_update_get_error(const WlanFwUpdate* u);
const char* wlan_fw_update_get_remote_version(const WlanFwUpdate* u);
bool wlan_fw_update_is_running(const WlanFwUpdate* u);
uint32_t wlan_fw_update_get_speed_kbps(const WlanFwUpdate* u);
uint32_t wlan_fw_update_get_bytes_done(const WlanFwUpdate* u);
uint32_t wlan_fw_update_get_bytes_total(const WlanFwUpdate* u);
