#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t value;
    const char* name;
} BleSpamLovespouseMode;

size_t ble_spam_lovespouse_mode_count(void);
const BleSpamLovespouseMode* ble_spam_lovespouse_mode_at(size_t index);
const BleSpamLovespouseMode* ble_spam_lovespouse_find_mode(uint32_t value);
