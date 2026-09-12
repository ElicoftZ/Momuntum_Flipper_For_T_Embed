#pragma once

#include <stdbool.h>

/** Persisted (NVS) choice of OTA/SD-update source: Sor3nt-Upstream vs. der
 *  Momuntum-Fork. Wird von wlan_fw_update und wlan_sd_update genutzt (siehe
 *  wlan_*_set_source) sowie über die Settings-Scene geändert. Default (nie
 *  gesetzt) ist Momuntum. */

bool wlan_update_source_get_sor3nt(void);
void wlan_update_source_set_sor3nt(bool use_sor3nt);
