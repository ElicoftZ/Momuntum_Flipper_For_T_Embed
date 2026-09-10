#pragma once

#include <gui/scene_manager.h>

#define ADD_SCENE(prefix, name, id) HotspotArcadeScene##id,
typedef enum {
#include "hotspot_arcade_scene_config.h"
    HotspotArcadeSceneNum,
} HotspotArcadeScene;
#undef ADD_SCENE

extern const SceneManagerHandlers hotspot_arcade_scene_handlers;

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_enter(void* context);
#include "hotspot_arcade_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) \
    bool prefix##_scene_##name##_on_event(void* context, SceneManagerEvent event);
#include "hotspot_arcade_scene_config.h"
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) void prefix##_scene_##name##_on_exit(void* context);
#include "hotspot_arcade_scene_config.h"
#undef ADD_SCENE
