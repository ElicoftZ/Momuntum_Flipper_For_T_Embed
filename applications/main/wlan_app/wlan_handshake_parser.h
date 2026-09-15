#pragma once

#include <stdint.h>
#include <stdbool.h>

/** Beacon-Frame? (Mgmt type 0, subtype 8) */
bool wlan_hs_is_beacon(const uint8_t* payload, int len);

/** 802.11-Adressen je nach toDS/fromDS-Flags. Setzt header_len. */
bool wlan_hs_parse_addresses(
    const uint8_t* payload, int len,
    const uint8_t** bssid, const uint8_t** station, const uint8_t** ap,
    int* header_len);

/** LLC/SNAP mit EAPOL-EtherType (0x888E)? */
bool wlan_hs_is_eapol(const uint8_t* payload, int header_len, int len);

/** EAPOL-Key Message-Nummer (1..4) anhand KeyInfo-Bitfield. 0 wenn ungültig. */
uint8_t wlan_hs_get_eapol_msg_num(const uint8_t* eapol_start, int eapol_len);

/** SSID aus Beacon-Tagged-Parameters extrahieren. */
bool wlan_hs_extract_beacon_ssid(const uint8_t* payload, int len, char* ssid_out, int max_len);

#define WLAN_HS_PMKID_LEN 16

/** Extract the PMKID from an EAPOL message-1 frame's Key Data field (the
 *  RSN PMKID KDE: vendor-specific IE 0xDD, OUI 00:0F:AC, data type 4, 16
 *  bytes), if present. This is what makes PMKID capture possible without a
 *  connecting client -- the AP includes it unauthenticated in message 1 of
 *  the 4-way handshake so a roaming client can skip full 802.1X. Caller must
 *  already know eapol_start/eapol_len is message 1 (wlan_hs_get_eapol_msg_num
 *  == 1); this does not re-check the message number itself. */
bool wlan_hs_extract_pmkid(const uint8_t* eapol_start, int eapol_len, uint8_t* pmkid_out);
