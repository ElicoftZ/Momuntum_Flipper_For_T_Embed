#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// AirSnitch L3-Probe (Gateway-Bounce): testet vom aktuellen (Gast-)STA aus die
// Erreichbarkeit von Geräten in gängigen Fremd-Subnetzen. LwIP routet Pakete an
// nicht-lokale Ziel-IPs automatisch über das Default-Gateway; kommt ein SYN-ACK
// oder RST zurück, hat der Router Gast→Privat geroutet → Client-Isolation
// umgangen. Reine TCP-Connect-Liveness, keine Roh-Frame-Injection.
//
// Threading: der Worker läuft als echter FreeRTOS-Task (xTaskCreate), weil
// lwIP-Sockets nicht aus FuriThreads getrieben werden dürfen.

/** Setzt den Probe-Zustand zurück (Ergebnisse, Progress, Flags). */
void wlan_airsnitch_reset(void);

/** Startet den L3-Probe-Worker. Idempotent (kein Effekt wenn bereits laufend). */
void wlan_airsnitch_start(void);

/** true sobald der Worker durchgelaufen ist. */
bool wlan_airsnitch_is_done(void);

/** Stoppt einen laufenden Worker und blockiert bis er beendet ist. Idempotent. */
void wlan_airsnitch_stop(void);

/** Fortschritt 0..100. */
uint8_t wlan_airsnitch_get_progress(void);

/** Anzahl bisher gefundener erreichbarer Fremd-Subnetz-Hosts. */
uint8_t wlan_airsnitch_get_count(void);

/** Liefert die IP (Network-Byte-Order) des gefundenen Hosts an idx. */
bool wlan_airsnitch_get(uint8_t idx, uint32_t* ip_be_out);

/** Kopiert einen kurzen Status (gerade geprüftes Subnetz, z.B. "192.168.178.x")
 *  nach out. */
void wlan_airsnitch_get_status(char* out, size_t cap);
