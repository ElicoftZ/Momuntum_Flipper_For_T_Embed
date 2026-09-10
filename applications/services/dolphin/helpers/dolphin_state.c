#include "dolphin_state.h"
#include "dolphin_state_filename.h"

#include <furi.h>
#include <furi_hal.h>

#include <storage/storage.h>
#include <toolbox/saved_struct.h>

#define TAG "DolphinState"

#define DOLPHIN_STATE_PATH           INT_PATH(DOLPHIN_STATE_FILE_NAME)
#define DOLPHIN_STATE_HEADER_MAGIC 0xD0
/* v1 was OFW's 3-level scale (level 2 at 300, level 3 at 1800). v2 is
 * Momentum's 30-level table below. DolphinStoreData is byte-identical
 * between them -- only the meaning of icounter changed -- so a v1 file is
 * migrated by re-stamping it, keeping the XP value untouched. */
#define DOLPHIN_STATE_HEADER_VERSION    0x02
#define DOLPHIN_STATE_HEADER_VERSION_V1 0x01
#define BUTTHURT_MAX                    14
#define BUTTHURT_MIN                    0

const uint32_t DOLPHIN_LEVELS[] = {100,  200,  300,  450,  600,  750,  950,  1150, 1350, 1600,
                                   1850, 2100, 2400, 2700, 3000, 3350, 3700, 4050, 4450, 4850,
                                   5250, 5700, 6150, 6600, 7100, 7600, 8100, 8650, 9999};
const size_t DOLPHIN_LEVEL_COUNT = COUNT_OF(DOLPHIN_LEVELS);

DolphinState* dolphin_state_alloc(void) {
    return calloc(1, sizeof(DolphinState));
}

void dolphin_state_free(DolphinState* dolphin_state) {
    free(dolphin_state);
}

void dolphin_state_save(DolphinState* dolphin_state) {
    if(!dolphin_state->dirty) {
        return;
    }

    bool success = saved_struct_save(
        DOLPHIN_STATE_PATH,
        &dolphin_state->data,
        sizeof(DolphinStoreData),
        DOLPHIN_STATE_HEADER_MAGIC,
        DOLPHIN_STATE_HEADER_VERSION);

    if(success) {
        FURI_LOG_I(TAG, "State saved");
        dolphin_state->dirty = false;

    } else {
        FURI_LOG_E(TAG, "Failed to save state");
    }
}

/* Adopt a state file written by the OFW-scale build. saved_struct_load checks
 * the version and refuses a mismatch, so without this every existing dolphin
 * would be silently reset to zero on the first boot after the upgrade.
 *
 * The payload is unchanged, and so is the XP value: the new table simply reads
 * it on a finer scale, so a dolphin keeps its icounter and gains level numbers. */
static bool dolphin_state_load_v1(DolphinState* dolphin_state) {
    uint8_t magic = 0;
    uint8_t version = 0;
    size_t payload_size = 0;

    if(!saved_struct_get_metadata(DOLPHIN_STATE_PATH, &magic, &version, &payload_size)) {
        return false;
    }
    if(magic != DOLPHIN_STATE_HEADER_MAGIC ||
       version != DOLPHIN_STATE_HEADER_VERSION_V1) {
        return false;
    }
    if(!saved_struct_load(
           DOLPHIN_STATE_PATH,
           &dolphin_state->data,
           sizeof(DolphinStoreData),
           DOLPHIN_STATE_HEADER_MAGIC,
           DOLPHIN_STATE_HEADER_VERSION_V1)) {
        return false;
    }

    FURI_LOG_I(
        TAG,
        "Migrated state to the Momentum XP scale: %lu XP is now level %u",
        (unsigned long)dolphin_state->data.icounter,
        dolphin_get_level(dolphin_state->data.icounter));

    /* Re-save so the file carries the current version. */
    dolphin_state->dirty = true;
    return true;
}

void dolphin_state_load(DolphinState* dolphin_state) {
    bool success = saved_struct_load(
        DOLPHIN_STATE_PATH,
        &dolphin_state->data,
        sizeof(DolphinStoreData),
        DOLPHIN_STATE_HEADER_MAGIC,
        DOLPHIN_STATE_HEADER_VERSION);

    if(!success) {
        success = dolphin_state_load_v1(dolphin_state);
    }

    if(success) {
        if((dolphin_state->data.butthurt > BUTTHURT_MAX) ||
           (dolphin_state->data.butthurt < BUTTHURT_MIN)) {
            success = false;
        }

        /* This field is retained only to keep existing state files byte-for-
         * byte compatible. Calendar timestamps no longer drive Dolphin logic. */
        if(success && dolphin_state->data.timestamp != 0) {
            dolphin_state->data.timestamp = 0;
            dolphin_state->dirty = true;
        }
    }

    if(!success) {
        FURI_LOG_W(TAG, "Reset Dolphin state");
        memset(dolphin_state, 0, sizeof(DolphinState));

        dolphin_state->dirty = true;
        dolphin_state_save(dolphin_state);
    }
}

bool dolphin_state_is_levelup(uint32_t icounter) {
    for(size_t i = 0; i < DOLPHIN_LEVEL_COUNT; ++i) {
        if(icounter == DOLPHIN_LEVELS[i]) {
            return true;
        }
    }
    return false;
}

uint8_t dolphin_get_level(uint32_t icounter) {
    for(size_t i = 0; i < DOLPHIN_LEVEL_COUNT; ++i) {
        if(icounter <= DOLPHIN_LEVELS[i]) {
            return i + 1;
        }
    }
    return DOLPHIN_LEVEL_COUNT + 1;
}

uint32_t dolphin_state_xp_above_last_levelup(uint32_t icounter) {
    uint8_t level_idx = dolphin_get_level(icounter) - 1; // Level = index + 1
    if(level_idx > 0) {
        return icounter - DOLPHIN_LEVELS[level_idx - 1]; // Get prev level
    }
    return icounter;
}

uint32_t dolphin_state_xp_to_levelup(uint32_t icounter) {
    uint8_t level_idx = dolphin_get_level(icounter) - 1; // Level = index + 1
    if(level_idx < DOLPHIN_LEVEL_COUNT) {
        return DOLPHIN_LEVELS[level_idx] - icounter;
    }
    return (uint32_t)-1;
}

void dolphin_state_on_deed(DolphinState* dolphin_state, DolphinDeed deed) {
    // Special case for testing
    if(deed > DolphinDeedMAX) {
        if(deed == DolphinDeedTestLeft) {
            dolphin_state->data.butthurt =
                CLAMP(dolphin_state->data.butthurt + 1, BUTTHURT_MAX, BUTTHURT_MIN);
            if(dolphin_state->data.icounter > 0) dolphin_state->data.icounter--;
            dolphin_state->dirty = true;
        } else if(deed == DolphinDeedTestRight) {
            dolphin_state->data.butthurt = BUTTHURT_MIN;
            if(dolphin_state->data.icounter < UINT32_MAX) dolphin_state->data.icounter++;
            dolphin_state->dirty = true;
        }
        return;
    }

    DolphinApp app = dolphin_deed_get_app(deed);
    int8_t weight_limit =
        dolphin_deed_get_app_limit(app) - dolphin_state->data.icounter_daily_limit[app];
    uint8_t deed_weight = CLAMP(dolphin_deed_get_weight(deed), weight_limit, 0);

    uint32_t xp_to_levelup = dolphin_state_xp_to_levelup(dolphin_state->data.icounter);
    if(xp_to_levelup) {
        deed_weight = MIN(xp_to_levelup, deed_weight);
        dolphin_state->data.icounter += deed_weight;
        dolphin_state->data.icounter_daily_limit[app] += deed_weight;
    }

    /* decrease butthurt:
     * 0 deeds accumulating --> 0 butthurt
     * +1....+15 deeds accumulating --> -1 butthurt
     * +16...+30 deeds accumulating --> -1 butthurt
     * +31...+45 deeds accumulating --> -1 butthurt
     * +46...... deeds accumulating --> -1 butthurt
     * -4 butthurt per day is maximum
     * */
    uint8_t butthurt_icounter_level_old = dolphin_state->data.butthurt_daily_limit / 15 +
                                          !!(dolphin_state->data.butthurt_daily_limit % 15);
    dolphin_state->data.butthurt_daily_limit =
        CLAMP(dolphin_state->data.butthurt_daily_limit + deed_weight, 46, 0);
    uint8_t butthurt_icounter_level_new = dolphin_state->data.butthurt_daily_limit / 15 +
                                          !!(dolphin_state->data.butthurt_daily_limit % 15);
    int32_t new_butthurt = ((int32_t)dolphin_state->data.butthurt) -
                           (butthurt_icounter_level_old != butthurt_icounter_level_new);
    new_butthurt = CLAMP(new_butthurt, BUTTHURT_MAX, BUTTHURT_MIN);

    dolphin_state->data.butthurt = new_butthurt;
    dolphin_state->data.timestamp = 0;
    dolphin_state->dirty = true;

    FURI_LOG_D(
        TAG,
        "icounter %lu, butthurt %ld",
        dolphin_state->data.icounter,
        dolphin_state->data.butthurt);
}

void dolphin_state_butthurted(DolphinState* dolphin_state) {
    if(dolphin_state->data.butthurt < BUTTHURT_MAX) {
        dolphin_state->data.butthurt++;
        dolphin_state->data.timestamp = 0;
        dolphin_state->dirty = true;
    }
}

void dolphin_state_increase_level(DolphinState* dolphin_state) {
    furi_assert(dolphin_state_is_levelup(dolphin_state->data.icounter));
    ++dolphin_state->data.icounter;
    dolphin_state->dirty = true;
}

void dolphin_state_clear_limits(DolphinState* dolphin_state) {
    furi_assert(dolphin_state);

    for(int i = 0; i < DolphinAppMAX; ++i) {
        dolphin_state->data.icounter_daily_limit[i] = 0;
    }
    dolphin_state->data.butthurt_daily_limit = 0;
    dolphin_state->dirty = true;
}
