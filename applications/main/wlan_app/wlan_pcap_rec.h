#pragma once

#include <stdbool.h>
#include <stdint.h>

// Asynchroner PCAP-Recorder für den Packet Sniffer.
//
// Der Promiscuous-Callback (WiFi-Task-Kontext) ruft nur wlan_pcap_rec_frame()
// auf — das kopiert den Frame in einen PSRAM-Ringpuffer und kehrt sofort zurück.
// Ein eigener Writer-Thread (FuriThread) leert den Ring und schreibt die Frames
// über die wlan_pcap_*-Primitive (DLT 105, IEEE802.11) auf die SD. So blockiert
// der WiFi-Treiber nie auf dem (langsamen) SD-Zugriff.
//
// Threading-Contract: wlan_pcap_rec_frame() darf NUR aus dem Promiscuous-
// Callback kommen (Single-Producer). start/stop/getter laufen im UI-Thread. Der
// Aufrufer MUSS Promiscuous deaktivieren (Callback aus), BEVOR er
// wlan_pcap_rec_stop() ruft.

// Öffnet die Datei (pcap-Header), legt Ring + Writer-Thread an.
// false bei Fehler (SD fehlt, kein PSRAM …).
bool wlan_pcap_rec_start(const char* path);

// Reserve the worker before WiFi/GUI consume internal RAM; release on app exit.
bool wlan_pcap_rec_prepare(void);
void wlan_pcap_rec_release(void);

// Stoppt den Writer, schreibt den Ring leer und schließt die Datei.
void wlan_pcap_rec_stop(void);

// Frame in den Ring stellen (nur aus dem Promiscuous-Callback). len = sig_len.
void wlan_pcap_rec_frame(const uint8_t* payload, uint16_t len);

bool wlan_pcap_rec_is_active(void);
uint32_t wlan_pcap_rec_frames(void); // tatsächlich auf SD geschriebene Frames
uint32_t wlan_pcap_rec_bytes(void);  // Dateigröße in Bytes
uint32_t wlan_pcap_rec_drops(void);  // verworfene Frames (Ring war voll)
const char* wlan_pcap_rec_path(void);
