#include "hotspot_arcade_i.h"

#include <stdlib.h>
#include <string.h>

static const NotificationSequence hotspot_arcade_sound_sequence = {
    &message_note_c5,
    &message_delay_100,
    &message_sound_off,
    NULL,
};

static const NotificationSequence hotspot_arcade_vibro_sequence = {
    &message_vibro_on,
    &message_delay_50,
    &message_vibro_off,
    NULL,
};

static const char* const hotspot_arcade_game_names[] = {
    "No game",
    "Trivia",
    "Connect Four",
    "Tic-Tac-Toe",
    "Dots & Boxes",
    "Draw & Guess",
    "Pong",
    "Reaction Duel",
    "Would You Rather",
    "Word Scramble",
    "Reversi",
    "Guess the Color",
    "Battleship",
    "Spectrum",
    "Kiss Marry Kill",
    "Chess",
    "Secrets",
    "Fill the Blank",
    "Werewolf",
    "Spyfall",
    "Draw a Monster",
    "Rock Paper Scissors",
    "Math Rush",
    "Simon Says",
    "Who Is The Impostor",
    "Bulls & Cows",
    "2048",
    "Snake",
    "Minesweeper",
    "Memory Match",
    "15 Puzzle",
    "Higher or Lower",
    "Aim Trainer",
    "Odd One Out",
    "Liar's Dice",
    "Word Bomb",
    "Nim",
    "Gomoku",
    "Categories",
    "Bid Wars",
    "Tug of War",
};

const char* hotspot_arcade_game_name(uint8_t game_id) {
    if(game_id >= COUNT_OF(hotspot_arcade_game_names)) return "Unknown";
    return hotspot_arcade_game_names[game_id];
}

static void hotspot_arcade_snapshot_terminate_strings(HotspotArcadeServiceSnapshot* snapshot) {
    snapshot->ssid[sizeof(snapshot->ssid) - 1U] = '\0';
    snapshot->ip[sizeof(snapshot->ip) - 1U] = '\0';
    snapshot->last_event[sizeof(snapshot->last_event) - 1U] = '\0';
    for(size_t index = 0; index < HOTSPOT_ARCADE_SERVICE_MAX_PLAYERS; index++) {
        snapshot->players[index].nick[sizeof(snapshot->players[index].nick) - 1U] = '\0';
    }
}

bool hotspot_arcade_refresh_snapshot(HotspotArcadeApp* app) {
    furi_check(app);

    HotspotArcadeServiceSnapshot next = {0};
    if(hotspot_arcade_service_snapshot(&next)) {
        hotspot_arcade_snapshot_terminate_strings(&next);
        const bool changed = !app->have_snapshot ||
                             (next.revision != app->snapshot.revision) ||
                             (next.running != app->snapshot.running);
        if(changed) {
            app->snapshot = next;
            app->have_snapshot = true;
        }
        return changed;
    }

    const bool running = hotspot_arcade_service_is_running();
    if(!app->have_snapshot || app->snapshot.running != running) {
        memset(&app->snapshot, 0, sizeof(app->snapshot));
        app->snapshot.running = running;
        app->have_snapshot = true;
        return true;
    }
    return false;
}

void hotspot_arcade_show_message(
    HotspotArcadeApp* app,
    const char* title,
    const char* body) {
    furi_check(app);
    furi_string_set(app->message_title, title ? title : "Hotspot Arcade");
    furi_string_set(app->message_body, body ? body : "");
    scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneMessage);
}

void hotspot_arcade_feedback(HotspotArcadeApp* app) {
    furi_check(app);
    if(app->settings.sound) {
        notification_message(app->notifications, &hotspot_arcade_sound_sequence);
    }
    if(app->settings.vibro) {
        notification_message(app->notifications, &hotspot_arcade_vibro_sequence);
    }
}

static bool hotspot_arcade_custom_event_callback(void* context, uint32_t event) {
    HotspotArcadeApp* app = context;
    return scene_manager_handle_custom_event(app->scene_manager, event);
}

static bool hotspot_arcade_back_event_callback(void* context) {
    HotspotArcadeApp* app = context;
    if(!scene_manager_handle_back_event(app->scene_manager)) {
        view_dispatcher_stop(app->view_dispatcher);
    }
    return true;
}

static void hotspot_arcade_tick_event_callback(void* context) {
    HotspotArcadeApp* app = context;
    if(hotspot_arcade_refresh_snapshot(app)) {
        /* The tick callback runs on the ViewDispatcher thread. Updating scene-owned
         * widgets here avoids cross-thread UI access and avoids a service callback. */
        scene_manager_handle_custom_event(app->scene_manager, HotspotArcadeEventSnapshotChanged);
    }
    scene_manager_handle_tick_event(app->scene_manager);
}

static HotspotArcadeApp* hotspot_arcade_alloc(void) {
    HotspotArcadeApp* app = malloc(sizeof(HotspotArcadeApp));
    furi_check(app);
    memset(app, 0, sizeof(*app));

    app->gui = furi_record_open(RECORD_GUI);
    app->storage = furi_record_open(RECORD_STORAGE);
    app->notifications = furi_record_open(RECORD_NOTIFICATION);

    if(!hotspot_arcade_settings_load(app->storage, &app->settings)) {
        /* This is best effort: an absent SD card is reported when Start is chosen. */
        hotspot_arcade_settings_save(app->storage, &app->settings);
    }

    app->console_buffer = malloc(HOTSPOT_ARCADE_CONSOLE_BUFFER_SIZE);
    furi_check(app->console_buffer);
    app->console_buffer[0] = '\0';
    app->display_text = furi_string_alloc();
    app->message_title = furi_string_alloc();
    app->message_body = furi_string_alloc();

    app->view_dispatcher = view_dispatcher_alloc();
    app->scene_manager = scene_manager_alloc(&hotspot_arcade_scene_handlers, app);
    view_dispatcher_set_event_callback_context(app->view_dispatcher, app);
    view_dispatcher_set_custom_event_callback(
        app->view_dispatcher, hotspot_arcade_custom_event_callback);
    view_dispatcher_set_navigation_event_callback(
        app->view_dispatcher, hotspot_arcade_back_event_callback);
    view_dispatcher_set_tick_event_callback(
        app->view_dispatcher, hotspot_arcade_tick_event_callback, 500U);

    app->submenu = submenu_alloc();
    app->text_input = text_input_alloc();
    app->widget = widget_alloc();
    view_dispatcher_add_view(
        app->view_dispatcher, HotspotArcadeViewSubmenu, submenu_get_view(app->submenu));
    view_dispatcher_add_view(
        app->view_dispatcher,
        HotspotArcadeViewTextInput,
        text_input_get_view(app->text_input));
    view_dispatcher_add_view(
        app->view_dispatcher, HotspotArcadeViewWidget, widget_get_view(app->widget));
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    hotspot_arcade_refresh_snapshot(app);
    scene_manager_next_scene(app->scene_manager, HotspotArcadeSceneMain);
    return app;
}

static void hotspot_arcade_free(HotspotArcadeApp* app) {
    furi_check(app);

    /* The S3 service owns Wi-Fi and its worker tasks. It must be fully stopped
     * before this FAP releases any storage or UI state. */
    hotspot_arcade_service_stop();

    view_dispatcher_remove_view(app->view_dispatcher, HotspotArcadeViewSubmenu);
    view_dispatcher_remove_view(app->view_dispatcher, HotspotArcadeViewTextInput);
    view_dispatcher_remove_view(app->view_dispatcher, HotspotArcadeViewWidget);
    submenu_free(app->submenu);
    text_input_free(app->text_input);
    widget_free(app->widget);
    scene_manager_free(app->scene_manager);
    view_dispatcher_free(app->view_dispatcher);

    furi_string_free(app->message_body);
    furi_string_free(app->message_title);
    furi_string_free(app->display_text);
    free(app->console_buffer);

    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_STORAGE);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t hotspot_arcade_app(void* argument) {
    UNUSED(argument);
    HotspotArcadeApp* app = hotspot_arcade_alloc();
    view_dispatcher_run(app->view_dispatcher);
    hotspot_arcade_free(app);
    return 0;
}
