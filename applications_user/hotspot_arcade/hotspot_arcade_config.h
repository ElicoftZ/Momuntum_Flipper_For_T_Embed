#pragma once

#include <stdbool.h>
#include <storage/storage.h>

#define HOTSPOT_ARCADE_SSID_MAX_LENGTH 32U
#define HOTSPOT_ARCADE_LANG_MAX_LENGTH 7U

#define HOTSPOT_ARCADE_CONFIG_DIR EXT_PATH("apps_data/hotspot_arcade")
#define HOTSPOT_ARCADE_CONFIG_PATH HOTSPOT_ARCADE_CONFIG_DIR "/config.txt"

typedef struct {
    char ssid[HOTSPOT_ARCADE_SSID_MAX_LENGTH + 1U];
    char lang[HOTSPOT_ARCADE_LANG_MAX_LENGTH + 1U];
    bool sound;
    bool vibro;
} HotspotArcadeSettings;

void hotspot_arcade_settings_set_defaults(HotspotArcadeSettings* settings);
bool hotspot_arcade_settings_load(Storage* storage, HotspotArcadeSettings* settings);
bool hotspot_arcade_settings_save(Storage* storage, const HotspotArcadeSettings* settings);
