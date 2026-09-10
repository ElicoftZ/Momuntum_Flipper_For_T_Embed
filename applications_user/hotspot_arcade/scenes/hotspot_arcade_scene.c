#include "../hotspot_arcade_i.h"

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_enter,
static void (*const hotspot_arcade_scene_on_enter_handlers[])(void*) = {
#include "hotspot_arcade_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_event,
static bool (*const hotspot_arcade_scene_on_event_handlers[])(void*, SceneManagerEvent) = {
#include "hotspot_arcade_scene_config.h"
};
#undef ADD_SCENE

#define ADD_SCENE(prefix, name, id) prefix##_scene_##name##_on_exit,
static void (*const hotspot_arcade_scene_on_exit_handlers[])(void*) = {
#include "hotspot_arcade_scene_config.h"
};
#undef ADD_SCENE

const SceneManagerHandlers hotspot_arcade_scene_handlers = {
    .on_enter_handlers = hotspot_arcade_scene_on_enter_handlers,
    .on_event_handlers = hotspot_arcade_scene_on_event_handlers,
    .on_exit_handlers = hotspot_arcade_scene_on_exit_handlers,
    .scene_num = HotspotArcadeSceneNum,
};
