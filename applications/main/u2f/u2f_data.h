#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <furi.h>

bool u2f_data_check(bool cert_only);

bool u2f_data_cert_check(void);

/** Read the attestation certificate into `cert`, refusing anything longer than
 * `max_len`. The bound matters: u2f_data_cert_check() only validates that the
 * DER length field agrees with the file size, and a well-formed long-form DER
 * header can declare up to 64 KB -- so the file's own size decides how much is
 * written, and every caller has a much smaller buffer than that.
 * Returns the number of bytes read, or 0 on any failure. */
uint32_t u2f_data_cert_load(uint8_t* cert, uint32_t max_len);

bool u2f_data_cert_key_load(uint8_t* cert_key);

/** Try the key derivation used by early ESP32-port builds. */
bool u2f_data_cert_key_load_legacy(uint8_t* cert_key);

/** Load files written while the ESP32 crypto HAL was passthrough-only. */
bool u2f_data_cert_key_load_plaintext_legacy(uint8_t* cert_key);

/** Reseal a successfully migrated certificate key with the current key. */
bool u2f_data_cert_key_reencrypt(const uint8_t* cert_key);

bool u2f_data_key_load(uint8_t* device_key);

bool u2f_data_key_load_legacy(uint8_t* device_key);

bool u2f_data_key_load_plaintext_legacy(uint8_t* device_key);

bool u2f_data_key_reencrypt(const uint8_t* device_key);

bool u2f_data_key_generate(uint8_t* device_key);

bool u2f_data_cnt_read(uint32_t* cnt);

bool u2f_data_cnt_read_legacy(uint32_t* cnt);

bool u2f_data_cnt_read_plaintext_legacy(uint32_t* cnt);

bool u2f_data_cnt_write(uint32_t cnt);

#ifdef __cplusplus
}
#endif
