#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HOTSPOT_ARCADE_SERVICE_MAX_PLAYERS 12

typedef enum {
    HotspotArcadeServiceResultOk = 0,
    HotspotArcadeServiceResultInvalidArgument,
    HotspotArcadeServiceResultAlreadyRunning,
    HotspotArcadeServiceResultNotRunning,
    HotspotArcadeServiceResultStorageError,
    HotspotArcadeServiceResultNetworkError,
    HotspotArcadeServiceResultNoMemory,
    HotspotArcadeServiceResultInternalError,
} HotspotArcadeServiceResult;

typedef struct {
    const char* ssid;
    const char* web_gzip_path;
    const char* bundled_packs_dir;
    const char* user_packs_dir;
    const char* lang;
} HotspotArcadeServiceConfig;

typedef struct {
    bool used;
    uint8_t pid;
    char nick[20];
    int32_t score;
} HotspotArcadeServicePlayer;

typedef struct {
    uint32_t revision;
    bool running;
    uint8_t active_game;
    uint8_t player_count;
    uint8_t client_count;
    char ssid[33];
    char ip[16];
    char last_event[80];
    HotspotArcadeServicePlayer players[HOTSPOT_ARCADE_SERVICE_MAX_PLAYERS];
} HotspotArcadeServiceSnapshot;

HotspotArcadeServiceResult hotspot_arcade_service_start(
    const HotspotArcadeServiceConfig* config);
void hotspot_arcade_service_stop(void);
bool hotspot_arcade_service_is_running(void);
HotspotArcadeServiceResult hotspot_arcade_service_select_game(uint8_t game_id);
void hotspot_arcade_service_reset_scores(void);
void hotspot_arcade_service_round_end(void);
bool hotspot_arcade_service_snapshot(HotspotArcadeServiceSnapshot* out_snapshot);
size_t hotspot_arcade_service_copy_console(char* buffer, size_t buffer_size);
const char* hotspot_arcade_service_result_to_string(HotspotArcadeServiceResult result);

#ifdef __cplusplus
}
#endif

