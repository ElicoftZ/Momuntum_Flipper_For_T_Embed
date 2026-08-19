#include "ble_spam_lovespouse.h"

static const BleSpamLovespouseMode lovespouse_modes[] = {
    {0xE49C6C, "Classic 1"},
    {0xE7075E, "Classic 2"},
    {0xE68E4F, "Classic 3"},
    {0xE1313B, "Classic 4"},
    {0xE0B82A, "Classic 5"},
    {0xE32318, "Classic 6"},
    {0xE2AA09, "Classic 7"},
    {0xED5DF1, "Classic 8"},
    {0xECD4E0, "Classic 9"},
    {0xD41F5D, "Independent 1-1"},
    {0xD7846F, "Independent 1-2"},
    {0xD60D7E, "Independent 1-3"},
    {0xD1B20A, "Independent 1-4"},
    {0xD0B31B, "Independent 1-5"},
    {0xD3A029, "Independent 1-6"},
    {0xD22938, "Independent 1-7"},
    {0xDDDEC0, "Independent 1-8"},
    {0xDC57D1, "Independent 1-9"},
    {0xA4982E, "Independent 2-1"},
    {0xA7031C, "Independent 2-2"},
    {0xA68A0D, "Independent 2-3"},
    {0xA13579, "Independent 2-4"},
    {0xA0BC68, "Independent 2-5"},
    {0xA3275A, "Independent 2-6"},
    {0xA2AE4B, "Independent 2-7"},
    {0xAD59B3, "Independent 2-8"},
    {0xACD0A2, "Independent 2-9"},
    {0xE5157D, "Classic Stop"},
    {0xD5964C, "Independent 1 Stop"},
    {0xA5113F, "Independent 2 Stop"},
};

size_t ble_spam_lovespouse_mode_count(void) {
    return sizeof(lovespouse_modes) / sizeof(lovespouse_modes[0]);
}

const BleSpamLovespouseMode* ble_spam_lovespouse_mode_at(size_t index) {
    if(index >= ble_spam_lovespouse_mode_count()) return NULL;
    return &lovespouse_modes[index];
}

const BleSpamLovespouseMode* ble_spam_lovespouse_find_mode(uint32_t value) {
    for(size_t i = 0; i < ble_spam_lovespouse_mode_count(); i++) {
        if(lovespouse_modes[i].value == value) return &lovespouse_modes[i];
    }
    return NULL;
}
