#pragma once

#include <stdbool.h>
#include <stddef.h>

/** Prüft ob /ext/wifi/<ssid>.txt existiert (Pfad-unsichere Zeichen → "_"). */
bool wlan_password_exists(const char* ssid);

/** Liest das Passwort für SSID aus /ext/wifi/<ssid>.txt. Trim Whitespace.
 *  @return true wenn ≥ 1 Zeichen gelesen wurde. */
bool wlan_password_read(const char* ssid, char* out_pass, size_t max_len);

/** Speichert/überschreibt /ext/wifi/<ssid>.txt mit dem Passwort. */
bool wlan_password_save(const char* ssid, const char* password);

/** Remove a rejected saved password so the next connection asks again. */
bool wlan_password_delete(const char* ssid);

/** Remember/read the most recently connected SSID. The password remains in its
 *  existing per-SSID file; this marker only selects which network background
 *  WiFi should reconnect to. */
bool wlan_last_ssid_save(const char* ssid);
bool wlan_last_ssid_read(char* out_ssid, size_t max_len);
