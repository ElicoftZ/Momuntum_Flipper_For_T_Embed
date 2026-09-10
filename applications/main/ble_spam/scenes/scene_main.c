#include "../ble_spam_app.h"

enum MainMenuIndex {
    MainMenuIndexBleKeyboard,
    MainMenuIndexPresenter,
    MainMenuIndexMediaRemote,
    MainMenuIndexCameraShutter,
    MainMenuIndexMouse,
    MainMenuIndexMouseJiggler,
    MainMenuIndexBleSpam,
    MainMenuIndexBleWalk,
    MainMenuIndexBleAutoWalk,
    MainMenuIndexBleTracker,
    MainMenuIndexBleRaceDetector,
    MainMenuIndexWhisperPair,
};

static void main_menu_callback(void* context, uint32_t index) {
    BleSpamApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, index);
}

void ble_spam_scene_main_on_enter(void* context) {
    BleSpamApp* app = context;

    submenu_add_item(
        app->submenu, "BLE Keyboard", MainMenuIndexBleKeyboard, main_menu_callback, app);
    submenu_add_item(
        app->submenu, "Presenter", MainMenuIndexPresenter, main_menu_callback, app);
    submenu_add_item(
        app->submenu, "Media Remote", MainMenuIndexMediaRemote, main_menu_callback, app);
    submenu_add_item(
        app->submenu, "Camera Shutter", MainMenuIndexCameraShutter, main_menu_callback, app);
    submenu_add_item(app->submenu, "Mouse", MainMenuIndexMouse, main_menu_callback, app);
    submenu_add_item(
        app->submenu, "Mouse Jiggler", MainMenuIndexMouseJiggler, main_menu_callback, app);
    submenu_add_item(app->submenu, "BLE Spam", MainMenuIndexBleSpam, main_menu_callback, app);
    submenu_add_item(app->submenu, "Walk", MainMenuIndexBleWalk, main_menu_callback, app);
    submenu_add_item(
        app->submenu, "Auto Walk", MainMenuIndexBleAutoWalk, main_menu_callback, app);
    submenu_add_item(
        app->submenu, "Tracker", MainMenuIndexBleTracker, main_menu_callback, app);
    submenu_add_item(
        app->submenu,
        "Airoha RACE",
        MainMenuIndexBleRaceDetector,
        main_menu_callback,
        app);

    submenu_add_item(
        app->submenu, "WhisperPair", MainMenuIndexWhisperPair, main_menu_callback, app);

    view_dispatcher_switch_to_view(app->view_dispatcher, BleSpamViewSubmenu);
}

bool ble_spam_scene_main_on_event(void* context, SceneManagerEvent event) {
    BleSpamApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        switch(event.event) {
        case MainMenuIndexBleKeyboard:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneBleKeyboard);
            consumed = true;
            break;
        case MainMenuIndexPresenter:
            app->remote_mode = BleRemoteModePresenter;
            scene_manager_next_scene(app->scene_manager, BleSpamSceneBleRemote);
            consumed = true;
            break;
        case MainMenuIndexMediaRemote:
            app->remote_mode = BleRemoteModeMedia;
            scene_manager_next_scene(app->scene_manager, BleSpamSceneBleRemote);
            consumed = true;
            break;
        case MainMenuIndexCameraShutter:
            app->remote_mode = BleRemoteModeCamera;
            scene_manager_next_scene(app->scene_manager, BleSpamSceneBleRemote);
            consumed = true;
            break;
        case MainMenuIndexMouse:
            app->remote_mode = BleRemoteModeMouse;
            scene_manager_next_scene(app->scene_manager, BleSpamSceneBleRemote);
            consumed = true;
            break;
        case MainMenuIndexMouseJiggler:
            app->remote_mode = BleRemoteModeMouseJiggler;
            scene_manager_next_scene(app->scene_manager, BleSpamSceneBleRemote);
            consumed = true;
            break;
        case MainMenuIndexBleSpam:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneSpamMenu);
            consumed = true;
            break;
        case MainMenuIndexBleWalk:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneWalkScan);
            consumed = true;
            break;
        case MainMenuIndexBleAutoWalk:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneAutoWalk);
            consumed = true;
            break;
        case MainMenuIndexBleTracker:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneTrackerScan);
            consumed = true;
            break;
        case MainMenuIndexBleRaceDetector:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneRaceDetector);
            consumed = true;
            break;
        case MainMenuIndexWhisperPair:
            scene_manager_next_scene(app->scene_manager, BleSpamSceneWhisperPair);
            consumed = true;
            break;
        }
    }

    return consumed;
}

void ble_spam_scene_main_on_exit(void* context) {
    BleSpamApp* app = context;
    submenu_reset(app->submenu);
}
