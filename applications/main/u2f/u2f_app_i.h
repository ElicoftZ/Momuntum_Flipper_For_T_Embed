#pragma once

#include "u2f_app.h"
#include "scenes/u2f_scene.h"

#include <gui/gui.h>
#include <assets_icons.h>
#include <gui/view_dispatcher.h>
#include <gui/scene_manager.h>
#include <gui/modules/submenu.h>
#include <dialogs/dialogs.h>
#include <notification/notification_messages.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include "views/u2f_view.h"
#include "views/u2f_pin_input.h"
#include "ctap2_pin.h"
#include "ctap2_rk.h"
#include "u2f_hid.h"
#include "fido_nfc.h"
#include "u2f.h"

typedef enum {
    U2fAppErrorNoFiles,
    U2fAppErrorCloseRpc,
} U2fAppError;

typedef enum {
    U2fCustomEventNone,

    U2fCustomEventConnect,
    U2fCustomEventDisconnect,
    U2fCustomEventDataError,

    U2fCustomEventRegister,
    U2fCustomEventAuth,
    U2fCustomEventAuthSuccess,
    U2fCustomEventWink,

    U2fCustomEventTimeout,

    U2fCustomEventConfirm,

    U2fCustomEventErrorBack,

    U2fCustomEventOpenSettings,
    U2fCustomEventPinEntered,
    U2fCustomEventPinCancelled,

} GpioCustomEvent;

/* Which PIN operation the PIN scene is carrying out. */
typedef enum {
    U2fPinModeSet,
    U2fPinModeChange,
    U2fPinModeRemove,
} U2fPinMode;

/* Where in that operation it is. Set skips Current; Remove stops after it. */
typedef enum {
    U2fPinStageCurrent,
    U2fPinStageNew,
    U2fPinStageConfirm,
} U2fPinStage;

typedef enum {
    U2fSettingsIndexPin,
    U2fSettingsIndexRemovePin,
    U2fSettingsIndexCreds,
    U2fSettingsIndexRetries,
    U2fSettingsIndexReset,
} U2fSettingsIndex;

typedef enum {
    U2fAppViewError,
    U2fAppViewMain,
    U2fAppViewSettings,
    U2fAppViewPinInput,
} U2fAppView;

struct U2fApp {
    Gui* gui;
    ViewDispatcher* view_dispatcher;
    SceneManager* scene_manager;
    NotificationApp* notifications;
    Widget* widget;
    FuriTimer* timer;
    U2fHid* u2f_hid;
    FidoNfc* fido_nfc;
    U2fView* u2f_view;
    Submenu* submenu;
    U2fPinInput* pin_input;
    U2fData* u2f_instance;

    U2fPinMode pin_mode;
    U2fPinStage pin_stage;
    /* The PIN typed at the current step, plus the first of the two entries
     * while confirming a new one. */
    char pin_entry[CTAP2_PIN_LOCAL_MAX_LEN + 1];
    char pin_first[CTAP2_PIN_LOCAL_MAX_LEN + 1];
    /* Submenu labels are built at runtime (PIN state, retries left), and
     * submenu_add_item does not copy the string it is given. */
    char label_pin[32];
    char label_retries[32];
    char label_creds[32];
    char label_creds_header[32];
    /* The row the user picked in the saved-sites list, held across the two
     * confirmation steps. Kept whole because deleting needs (rp, user) and the
     * headers want the names. */
    Ctap2RkRecord creds_selected;
    char label_creds_item[CTAP2_RK_NAME_MAX * 2 + 4];
    GpioCustomEvent event_cur;
    bool u2f_ready;
    /* True while the main screen is answering over NFC instead of USB. */
    bool nfc_mode;
    U2fAppError error;
};
