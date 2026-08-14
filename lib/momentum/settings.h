#pragma once

#include "settings_core.h"

#include <storage/storage.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOMENTUM_SETTINGS_PATH INT_PATH(".momentum_settings.txt")

extern MomentumSettings momentum_settings;
extern uint32_t momentum_settings_revision;

void momentum_settings_load(void);
bool momentum_settings_save(void);

#ifdef __cplusplus
}
#endif
