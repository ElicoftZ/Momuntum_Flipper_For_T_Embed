#include "arm_fap_runtime.h"
#include "arm_fap_vm.h"
#include <esp_heap_caps.h>
#include <errno.h>
#include <bit_lib.h>
#include <toolbox/strint.h>
#include <toolbox/hex.h>
#include <toolbox/args.h>
#include <toolbox/path.h>
#include <toolbox/saved_struct.h>
#include <toolbox/pretty_format.h>
#include <toolbox/manchester_decoder.h>
#include <datetime/datetime.h>
#include <locale/locale.h>
#include <dolphin/dolphin.h>
#include <toolbox/bit_buffer.h>
#include <toolbox/dir_walk.h>
#include <toolbox/stream/file_stream.h>
#include <toolbox/stream/buffered_file_stream.h>
#include <math.h>
#include <stdlib.h>
#include <strings.h>
#include <furi.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <dialogs/dialogs.h>
#include <toolbox/compress.h>
#include <notification/notification_messages.h>
#include <furi_string.h>
#include <gui/elements.h>
#include <gui/view_holder.h>
#include <gui/modules/submenu.h>
#include <gui/modules/popup.h>
#include <gui/modules/loading.h>
#include <gui/modules/number_input.h>
#include <gui/modules/byte_input.h>
#include <gui/modules/menu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/variable_item_list.h>
#include <gui/modules/widget.h>
#include <gui/modules/text_input.h>
#include <gui/modules/dialog_ex.h>
#include <gui/scene_manager.h>
#include <infrared_worker.h>
#include <infrared_transmit.h>
#include <nfc/nfc.h>
#include <nfc/nfc_device.h>
#include <nfc/nfc_poller.h>
#include <nfc/nfc_scanner.h>
#include <nfc/protocols/mf_classic/mf_classic.h>
#include <nfc/protocols/mf_classic/mf_classic_poller_sync.h>
#include <nfc/protocols/mf_ultralight/mf_ultralight.h>
#include <nfc/protocols/iso15693_3/iso15693_3.h>
#include <storage/storage.h>
#include <flipper_format/flipper_format.h>
#include <flipper_format/flipper_format_i.h>
#include <toolbox/stream/stream.h>
#include <furi_hal_bt.h>
#include <furi_hal_crypto.h>
#include <furi_hal_infrared.h>
#include <furi_hal_nfc.h>
#include <furi_hal_power.h>
#include <furi_hal_rfid.h>
#include <furi_hal_rtc.h>
#include <furi_hal_speaker.h>
#include <furi_hal_usb.h>
#include <furi_hal_usb_hid.h>
#include <furi_hal_version.h>
#include <furi_hal_subghz.h>
#include "subghz_setting.h"
#include "subghz_worker.h"
#include "subghz_keystore.h"
#include "subghz_protocol_registry.h"
#include "environment.h"
#include "receiver.h"
#include "transmitter.h"
#include "blocks/generic.h"
#include "blocks/decoder.h"
#include "blocks/math.h"
#include "devices.h"
#include "device_registry.h"

#define TAG "ArmFap"
#define MAX_VIEWS 4u
#define VIEW_HANDLE 0xe0000000u
#define DISPATCHER_HANDLE 0xe1000000u
#define GUI_HANDLE 0xe2000000u
#define CANVAS_HANDLE 0xe3000000u
#define CALL_BUDGET 100000u
#define MAX_PORTS 2u
#define MAX_QUEUES 4u
#define PORT_HANDLE 0xe4000000u
#define QUEUE_HANDLE 0xe5000000u
#define NOTIFICATION_HANDLE 0xe6000000u
#define DIALOGS_HANDLE 0xef000000u
#define MAX_DIALOG_MESSAGES 2u
#define DIALOG_MESSAGE_HANDLE 0xf0000000u
#define MAX_DIALOG_EX 2u
#define DIALOGEX_HANDLE 0xf1000000u
#define MAX_SCENES 24u
#define MAX_SCENE_MANAGERS 1u
#define SCENE_MANAGER_HANDLE 0xf4000000u
#define INFRARED_HANDLE 0xf5000000u
#define SIGNAL_HANDLE 0xf6000000u
#define IR_MESSAGE_BUF_SIZE 16u
#define IR_RAW_BUF_COUNT MAX_TIMINGS_AMOUNT
#define MAX_NFC_PTRS 12u
#define NFC_PTR_HANDLE 0xf7000000u
typedef enum { NfcPtrNfc, NfcPtrDevice, NfcPtrPoller, NfcPtrScanner, NfcPtrData } NfcPtrKind;
#define MAX_STRINGS 8u
#define STRING_HANDLE 0xe7000000u
#define STRING_BUF_SIZE 256u
#define HOLDER_HANDLE 0xe8000000u
#define MAX_SUBMENUS 2u
#define SUBMENU_HANDLE 0xe9000000u
#define MAX_TEXTBOXES 2u
#define TEXTBOX_HANDLE 0xea000000u
#define MAX_VARLISTS 2u
#define VARLIST_HANDLE 0xeb000000u
#define MAX_ITEMS 24u
#define ITEM_HANDLE 0xec000000u
#define MAX_WIDGETS 2u
#define WIDGET_HANDLE 0xed000000u
#define MAX_TEXT_INPUTS 2u
#define TEXTINPUT_HANDLE 0xee000000u
#define STORAGE_HANDLE 0xf8000000u
#define MAX_FILES 8u
#define FILE_HANDLE 0xf9000000u
#define MAX_FORMATS 4u
#define FORMAT_HANDLE 0xfa000000u
#define MAX_STREAMS 4u
#define STREAM_HANDLE 0xfb000000u
#define FS_SCRATCH_SIZE 48u
#define MAX_MUTEXES 4u
#define MUTEX_HANDLE 0xfc000000u
#define MAX_SEMAPHORES 4u
#define SEMAPHORE_HANDLE 0xfd000000u
#define MAX_EVENT_FLAGS 4u
#define EVENT_FLAG_HANDLE 0xfe000000u
#define LOG_BUF_SIZE 160u
/* ARM builds use -fshort-enums, so the guest GapExtraBeaconConfig has 1-byte
 * enums (7 bytes of fields + 6-byte address) instead of the native 24 bytes. */
#define ARM_EXTRA_BEACON_CONFIG_SIZE 14u
#define CRYPTO_IV_SIZE 16u
#define HAL_UID_BUF_SIZE 16u
#define MAX_SUBGHZ_DEVICES 2u
#define SUBGHZ_DEVICE_HANDLE 0xff000000u
#define MAX_SUBGHZ_ENVS 2u
#define SUBGHZ_ENV_HANDLE 0xd8000000u
#define MAX_SUBGHZ_SETTINGS 2u
#define SUBGHZ_SETTING_HANDLE 0xd9000000u
#define MAX_SUBGHZ_RECEIVERS 2u
#define SUBGHZ_RECEIVER_HANDLE 0xda000000u
#define MAX_SUBGHZ_TRANSMITTERS 2u
#define SUBGHZ_TRANSMITTER_HANDLE 0xdb000000u
#define MAX_SUBGHZ_WORKERS 2u
#define SUBGHZ_WORKER_HANDLE 0xdc000000u
#define SUBGHZ_NAME_BUF_SIZE 48u
#define SUBGHZ_PRESET_BUF_SIZE 64u

typedef struct {
    ArmFapRuntime* owner;
    View* native;
    uint32_t model, context, draw, input, id;
    uint32_t enter, exit, previous;
    bool added;
    bool external; /* owned by a GUI module (Submenu/...); we never alloc/free it directly */
} ArmView;
typedef struct {
    ViewHolder* native;
    uint32_t back, back_context;
} ArmViewHolder;
typedef struct {
    ArmFapRuntime* owner;
    Submenu* native;
    uint32_t callback, context, view_handle;
} ArmSubmenu;
typedef struct {
    TextBox* native;
    uint32_t view_handle;
} ArmTextBox;
typedef struct {
    ArmFapRuntime* owner;
    VariableItemList* native;
    uint32_t view_handle, enter_callback, enter_context;
} ArmVarList;
typedef struct {
    ArmFapRuntime* owner;
    VariableItem* native;
    uint32_t handle, callback, context;
} ArmVariableItem;
typedef struct {
    ArmFapRuntime* owner;
    Widget* native;
    uint32_t view_handle, callback, context;
} ArmWidget;
typedef struct {
    ArmFapRuntime* owner;
    TextInput* native;
    uint32_t view_handle, result_callback, result_context;
} ArmTextInput;
typedef struct {
    DialogMessage* native;
} ArmDialogMessage;
typedef struct {
    ArmFapRuntime* owner;
    DialogEx* native;
    uint32_t view_handle, callback, context;
} ArmDialogEx;
typedef struct {
    ArmFapRuntime* owner;
    SceneManager* native;
    uint32_t context, scene_num;
    uint32_t on_enter[MAX_SCENES], on_event[MAX_SCENES], on_exit[MAX_SCENES];
} ArmSceneManager;
typedef struct {
    ArmFapRuntime* owner;
    InfraredWorker* native;
    uint32_t callback, context;
    uint32_t message_buf; /* IR_MESSAGE_BUF_SIZE guest scratch, for get_decoded_signal */
    uint32_t raw_buf; /* IR_RAW_BUF_COUNT*4 guest scratch, for get_raw_signal */
    uint32_t name_buf; /* small guest scratch, for infrared_get_protocol_name */
    const InfraredWorkerSignal* current_signal; /* valid only inside the RX trampoline */
    bool rx_running;
} ArmInfrared;
typedef struct {
    const void* native;
    uint8_t kind; /* NfcPtrKind */
    bool owned; /* true if we must free it; false for a borrowed/view pointer */
} ArmNfcPtr;

typedef struct {
    ArmFapRuntime* owner;
    ViewPort* native;
    uint32_t draw, input, draw_context, input_context;
    bool added;
} ArmPort;
typedef struct {
    FuriMessageQueue* native;
    uint32_t size;
    unsigned busy;
} ArmQueue;
typedef struct {
    FuriString* native;
    uint32_t guest_buf; /* STRING_BUF_SIZE-byte guest scratch, refreshed by get_cstr */
} ArmString;
typedef struct {
    File* native;
    bool is_dir;
} ArmFile;
typedef struct {
    FlipperFormat* native;
    uint32_t raw_stream; /* STREAM_HANDLE of the borrowed raw stream, or 0 */
} ArmFormat;
typedef struct {
    Stream* native;
    uint8_t kind; /* 0: generic/borrowed, 1: file, 2: buffered file */
    bool owned; /* false for flipper_format's raw stream: the format owns it */
} ArmStream;
typedef struct {
    FuriMutex* native;
    unsigned busy;
} ArmMutex;
typedef struct {
    FuriSemaphore* native;
    unsigned busy;
} ArmSemaphore;
typedef struct {
    FuriEventFlag* native;
} ArmEventFlag;
typedef struct {
    const SubGhzDevice* native;
} ArmSubGhzDevice;
typedef struct {
    SubGhzEnvironment* native;
} ArmSubGhzEnv;
typedef struct {
    SubGhzSetting* native;
} ArmSubGhzSetting;
typedef struct {
    SubGhzReceiver* native;
} ArmSubGhzReceiver;
typedef struct {
    SubGhzTransmitter* native;
} ArmSubGhzTransmitter;
typedef struct {
    ArmFapRuntime* owner;
    SubGhzWorker* native;
    uint32_t pair_callback, overrun_callback, context;
} ArmSubGhzWorker;

#define MAX_OTHER_MODULES 2u
typedef struct {
    ArmFapRuntime* owner;
    uint32_t callback, context;
    char* label;
} ArmMenuEntry;
#define POPUP_HANDLE 0xd0000000u
typedef struct {
    ArmFapRuntime* owner;
    Popup* native;
    uint32_t view_handle, callback, context;
    char* header; char* text;
} ArmPopup;
#define LOADING_HANDLE 0xd1000000u
typedef struct {
    ArmFapRuntime* owner;
    Loading* native;
    uint32_t view_handle, callback, context;
} ArmLoading;
#define NUMBER_INPUT_HANDLE 0xd2000000u
typedef struct {
    ArmFapRuntime* owner;
    NumberInput* native;
    uint32_t view_handle, callback, context;
    char* header;
} ArmNumberInput;
#define BYTE_INPUT_HANDLE 0xd3000000u
typedef struct {
    ArmFapRuntime* owner;
    ByteInput* native;
    uint32_t view_handle, callback, context;
    char* header; uint32_t changed, buffer, count; uint8_t bytes[255];
} ArmByteInput;
#define MENU_HANDLE 0xd4000000u
typedef struct {
    ArmFapRuntime* owner;
    Menu* native;
    uint32_t view_handle, callback, context;
    ArmMenuEntry entries[MAX_ITEMS]; unsigned count;
} ArmMenu;

#define MAX_BIT_BUFFERS 4u
#define BIT_BUFFER_HANDLE 0xd5000000u
#define MAX_DIR_WALKS 2u
#define DIR_WALK_HANDLE 0xd6000000u
typedef struct { BitBuffer* native; uint32_t guest_buf, capacity; } ArmBitBuffer;
typedef struct { DirWalk* native; bool opened; } ArmDirWalk;
struct ArmFapRuntime {
    ArmFapVm vm;
    ArmBitBuffer bit_buffers[MAX_BIT_BUFFERS];
    ArmDirWalk dir_walks[MAX_DIR_WALKS];
    ArmPopup popups[MAX_OTHER_MODULES];
    ArmLoading loadings[MAX_OTHER_MODULES];
    ArmNumberInput number_inputs[MAX_OTHER_MODULES];
    ArmByteInput byte_inputs[MAX_OTHER_MODULES];
    ArmMenu menus[MAX_OTHER_MODULES];

    FuriMutex* mutex;
    ArmView views[MAX_VIEWS];
    ViewDispatcher* dispatcher;
    uint32_t event_context, navigate, tick, event_buffer;
    Gui* gui;
    unsigned gui_refs;
    Canvas* canvas;
    Compress* compress;
    uint8_t bitmap[1024];
    bool running;
    ArmPort ports[MAX_PORTS];
    ArmQueue queues[MAX_QUEUES];
    ArmString strings[MAX_STRINGS];
    ArmFile files[MAX_FILES];
    ArmFormat formats[MAX_FORMATS];
    ArmStream streams[MAX_STREAMS];
    ArmMutex mutexes[MAX_MUTEXES];
    ArmSemaphore semaphores[MAX_SEMAPHORES];
    ArmEventFlag event_flags[MAX_EVENT_FLAGS];
    ArmSubGhzDevice subghz_devices[MAX_SUBGHZ_DEVICES];
    ArmSubGhzEnv subghz_envs[MAX_SUBGHZ_ENVS];
    ArmSubGhzSetting subghz_settings[MAX_SUBGHZ_SETTINGS];
    ArmSubGhzReceiver subghz_receivers[MAX_SUBGHZ_RECEIVERS];
    ArmSubGhzTransmitter subghz_transmitters[MAX_SUBGHZ_TRANSMITTERS];
    ArmSubGhzWorker subghz_workers[MAX_SUBGHZ_WORKERS];
    bool subghz_devices_owned;
    uint32_t subghz_name_buf, subghz_preset_buf;
    Storage* storage;
    unsigned storage_refs;
    uint32_t fs_scratch;
    uint32_t hal_uid_buf;
    uint32_t errno_buf;
    unsigned power_insomnia;
    ArmViewHolder holder;
    ArmSubmenu submenus[MAX_SUBMENUS];
    ArmTextBox text_boxes[MAX_TEXTBOXES];
    ArmVarList var_lists[MAX_VARLISTS];
    ArmVariableItem items[MAX_ITEMS];
    ArmWidget widgets[MAX_WIDGETS];
    ArmTextInput text_inputs[MAX_TEXT_INPUTS];
    ArmDialogMessage dialog_messages[MAX_DIALOG_MESSAGES];
    ArmDialogEx dialog_exs[MAX_DIALOG_EX];
    ArmSceneManager scene_managers[MAX_SCENE_MANAGERS];
    ArmInfrared infrared;
    ArmNfcPtr nfc_ptrs[MAX_NFC_PTRS];
    uint32_t nfc_name_buf, nfc_uid_buf, nfc_trailer_buf, nfc_scan_buf;
    uint32_t nfc_loading_callback, nfc_loading_context;
    uint32_t nfc_scanner_callback, nfc_scanner_context;
    DialogsApp* dialogs;
    unsigned dialogs_refs;
    uint32_t custom_event;
    bool direct_draw;
    NotificationApp* notification;
    unsigned notification_refs;
    unsigned gui_callback_depth;
    bool backlight_forced;
};

static void lock(ArmFapRuntime* a) { furi_mutex_acquire(a->mutex, FuriWaitForever); }
static void unlock(ArmFapRuntime* a) { furi_mutex_release(a->mutex); }

/* Never take the GUI lock while holding the interpreter lock: GUI draw
 * callbacks already own the GUI lock and enter the interpreter in that order.
 * A suspended guest call retains its registers across nested callbacks. */
#define GUI_CALL(statement) do { \
    if(a->gui_callback_depth) { fault(a, "Blocking operation in GUI callback"); return false; } \
    unlock(a); statement; lock(a); \
} while(0)

static void fault(ArmFapRuntime* a, const char* message) {
    arm_fap_vm_fault(&a->vm, message);
    if(a->running) view_dispatcher_stop(a->dispatcher);
}
static bool guest_call(ArmFapRuntime* a, uint32_t pc, const uint32_t* args, size_t n, uint32_t* result) {
    if(!pc || !arm_fap_vm_call(&a->vm, pc, args, n, CALL_BUDGET, result)) {
        fault(a, "Invalid ARM callback"); return false;
    }
    return true;
}
static void draw_callback(Canvas* canvas, void* model) {
    ArmView* v = *(ArmView**)model;
    ArmFapRuntime* a = v->owner;
    lock(a);
    a->gui_callback_depth++;
    a->canvas = canvas;
    const uint32_t args[] = {CANVAS_HANDLE, v->model};
    guest_call(a, v->draw, args, 2, NULL);
    a->canvas = NULL;
    a->gui_callback_depth--;
    unlock(a);
}

static void port_draw_callback(Canvas* canvas, void* context) {
    ArmPort* p = context;
    ArmFapRuntime* a = p->owner;
    lock(a);
    a->gui_callback_depth++;
    a->canvas = canvas;
    const uint32_t args[] = {CANVAS_HANDLE, p->draw_context};
    if(p->draw) guest_call(a, p->draw, args, 2, NULL);
    a->canvas = NULL;
    a->gui_callback_depth--;
    unlock(a);
}
static void port_input_callback(InputEvent* event, void* context) {
    ArmPort* p = context;
    ArmFapRuntime* a = p->owner;
    if(event->key != InputKeyUp && event->key != InputKeyDown &&
       event->key != InputKeyOk && event->key != InputKeyBack) return;
    lock(a);
    a->gui_callback_depth++;
    if(event->key == InputKeyBack && event->type == InputTypeLong) {
        fault(a, "ARM app stopped by Back");
        a->gui_callback_depth--;
        unlock(a);
        return;
    }
    arm_fap_vm_write(&a->vm, a->event_buffer, event->sequence, 4);
    arm_fap_vm_write(&a->vm, a->event_buffer + 4, event->key, 1);
    arm_fap_vm_write(&a->vm, a->event_buffer + 5, event->type, 1);
    const uint32_t args[] = {a->event_buffer, p->input_context};
    if(p->input) guest_call(a, p->input, args, 2, NULL);
    a->gui_callback_depth--;
    unlock(a);
}
static bool input_callback(InputEvent* event, void* context) {
    ArmView* v = context;
    ArmFapRuntime* a = v->owner;
    /* T-Embed's physical keys only. ARM uses -fshort-enums: the key/type
     * occupy one byte each after the 32-bit sequence, unlike native Xtensa. */
    if(event->key != InputKeyUp && event->key != InputKeyDown &&
       event->key != InputKeyOk && event->key != InputKeyBack) return false;
    lock(a);
    arm_fap_vm_write(&a->vm, a->event_buffer, event->sequence, 4);
    arm_fap_vm_write(&a->vm, a->event_buffer + 4, event->key, 1);
    arm_fap_vm_write(&a->vm, a->event_buffer + 5, event->type, 1);
    const uint32_t args[] = {a->event_buffer, v->context};
    uint32_t consumed = 0;
    guest_call(a, v->input, args, 2, &consumed);
    unlock(a);
    return consumed != 0;
}
static bool navigation_callback(void* context) {
    ArmFapRuntime* a = context;
    lock(a);
    uint32_t consumed = 0;
    guest_call(a, a->navigate, &a->event_context, 1, &consumed);
    unlock(a);
    return consumed != 0;
}
static void tick_callback(void* context) {
    ArmFapRuntime* a = context;
    lock(a);
    guest_call(a, a->tick, &a->event_context, 1, NULL);
    unlock(a);
}
static void enter_callback(void* context) {
    ArmView* v = context;
    ArmFapRuntime* a = v->owner;
    lock(a);
    if(v->enter) guest_call(a, v->enter, &v->context, 1, NULL);
    unlock(a);
}
static void exit_callback(void* context) {
    ArmView* v = context;
    ArmFapRuntime* a = v->owner;
    lock(a);
    if(v->exit) guest_call(a, v->exit, &v->context, 1, NULL);
    unlock(a);
}
static uint32_t previous_callback(void* context) {
    ArmView* v = context;
    ArmFapRuntime* a = v->owner;
    lock(a);
    uint32_t result = 0;
    if(v->previous) guest_call(a, v->previous, &v->context, 1, &result);
    unlock(a);
    return result;
}
static bool custom_event_callback(void* context, uint32_t event) {
    ArmFapRuntime* a = context;
    lock(a);
    const uint32_t args[] = {a->event_context, event};
    uint32_t consumed = 0;
    guest_call(a, a->custom_event, args, 2, &consumed);
    unlock(a);
    return consumed != 0;
}
static void holder_back_callback(void* context) {
    ArmFapRuntime* a = context;
    lock(a);
    if(a->holder.back) guest_call(a, a->holder.back, &a->holder.back_context, 1, NULL);
    unlock(a);
}
static void submenu_item_trampoline(void* context, uint32_t index) {
    ArmSubmenu* sub = context;
    ArmFapRuntime* a = sub->owner;
    lock(a);
    const uint32_t args[] = {sub->context, index};
    guest_call(a, sub->callback, args, 2, NULL);
    unlock(a);
}
static void var_list_enter_trampoline(void* context, uint32_t index) {
    ArmVarList* list = context;
    ArmFapRuntime* a = list->owner;
    lock(a);
    const uint32_t args[] = {list->enter_context, index};
    guest_call(a, list->enter_callback, args, 2, NULL);
    unlock(a);
}
static void variable_item_change_trampoline(VariableItem* item) {
    ArmVariableItem* it = variable_item_get_context(item);
    ArmFapRuntime* a = it->owner;
    lock(a);
    guest_call(a, it->callback, &it->handle, 1, NULL);
    unlock(a);
}
static void widget_button_trampoline(GuiButtonType result, InputType type, void* context) {
    ArmWidget* w = context;
    ArmFapRuntime* a = w->owner;
    lock(a);
    const uint32_t args[] = {(uint32_t)result, (uint32_t)type, w->context};
    guest_call(a, w->callback, args, 3, NULL);
    unlock(a);
}
static void text_input_result_trampoline(void* context) {
    ArmTextInput* ti = context;
    ArmFapRuntime* a = ti->owner;
    lock(a);
    if(ti->result_callback) guest_call(a, ti->result_callback, &ti->result_context, 1, NULL);
    unlock(a);
}
static void dialog_ex_result_trampoline(DialogExResult result, void* context) {
    ArmDialogEx* d = context;
    ArmFapRuntime* a = d->owner;
    lock(a);
    const uint32_t args[] = {(uint32_t)result, d->context};
    guest_call(a, d->callback, args, 2, NULL);
    unlock(a);
}
/* One shared trampoline per callback kind, looked up by the SceneManager's own
 * notion of "current scene" -- avoids needing MAX_SCENES distinct native
 * function pointers. */
static void scene_enter_trampoline(void* context) {
    ArmSceneManager* sm = context;
    ArmFapRuntime* a = sm->owner;
    uint32_t scene_id = scene_manager_get_current_scene(sm->native);
    if(scene_id >= sm->scene_num || !sm->on_enter[scene_id]) return;
    lock(a);
    guest_call(a, sm->on_enter[scene_id], &sm->context, 1, NULL);
    unlock(a);
}
static void scene_exit_trampoline(void* context) {
    ArmSceneManager* sm = context;
    ArmFapRuntime* a = sm->owner;
    uint32_t scene_id = scene_manager_get_current_scene(sm->native);
    if(scene_id >= sm->scene_num || !sm->on_exit[scene_id]) return;
    lock(a);
    guest_call(a, sm->on_exit[scene_id], &sm->context, 1, NULL);
    unlock(a);
}
static bool scene_event_trampoline(void* context, SceneManagerEvent event) {
    ArmSceneManager* sm = context;
    ArmFapRuntime* a = sm->owner;
    uint32_t scene_id = scene_manager_get_current_scene(sm->native);
    if(scene_id >= sm->scene_num || !sm->on_event[scene_id]) return false;
    lock(a);
    const uint32_t args[] = {sm->context, (uint32_t)event.type, event.event};
    uint32_t consumed = 0;
    guest_call(a, sm->on_event[scene_id], args, 3, &consumed);
    unlock(a);
    return consumed != 0;
}
static void nfc_loading_trampoline(void* context, bool state) {
    ArmFapRuntime* a = context;
    lock(a);
    const uint32_t args[] = {a->nfc_loading_context, state ? 1u : 0u};
    guest_call(a, a->nfc_loading_callback, args, 2, NULL);
    unlock(a);
}
static void nfc_scanner_trampoline(NfcScannerEvent event, void* context) {
    ArmFapRuntime* a = context;
    lock(a);
    uint32_t count = event.data.protocol_num;
    if(count > 8) count = 8;
    uint32_t* dest = count ? arm_fap_vm_pointer(&a->vm, a->nfc_scan_buf, count * 4, true) : NULL;
    if(dest) for(uint32_t i = 0; i < count; i++) dest[i] = (uint32_t)event.data.protocols[i];
    const uint32_t args[] = {(uint32_t)event.type, dest ? count : 0, a->nfc_scan_buf,
                              a->nfc_scanner_context};
    guest_call(a, a->nfc_scanner_callback, args, 4, NULL);
    unlock(a);
}
static void infrared_rx_trampoline(void* context, InfraredWorkerSignal* signal) {
    ArmInfrared* ir = context;
    ArmFapRuntime* a = ir->owner;
    lock(a);
    ir->current_signal = signal;
    const uint32_t args[] = {ir->context, SIGNAL_HANDLE};
    guest_call(a, ir->callback, args, 2, NULL);
    ir->current_signal = NULL;
    unlock(a);
}
#define ARM_FAP_X32(x) x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x
static const AppSceneOnEnterCallback SCENE_ENTER_TABLE[MAX_SCENES] = {
    ARM_FAP_X32(scene_enter_trampoline)};
static const AppSceneOnEventCallback SCENE_EVENT_TABLE[MAX_SCENES] = {
    ARM_FAP_X32(scene_event_trampoline)};
static const AppSceneOnExitCallback SCENE_EXIT_TABLE[MAX_SCENES] = {
    ARM_FAP_X32(scene_exit_trampoline)};
#undef ARM_FAP_X32
static ArmView* get_view(ArmFapRuntime* a, uint32_t handle) {
    uint32_t i = (handle - VIEW_HANDLE) / 4;
    if(handle < VIEW_HANDLE || (handle & 3) || i >= MAX_VIEWS || !a->views[i].native) {
        fault(a, "Invalid ARM view handle"); return NULL;
    }
    return &a->views[i];
}
static bool has_dispatcher(ArmFapRuntime* a, uint32_t handle) {
    if(handle == DISPATCHER_HANDLE && a->dispatcher) return true;
    fault(a, "Invalid ARM dispatcher handle"); return false;
}
static bool has_holder(ArmFapRuntime* a, uint32_t handle) {
    if(handle == HOLDER_HANDLE && a->holder.native) return true;
    fault(a, "Invalid ARM view holder handle"); return false;
}
static ArmSubmenu* get_submenu(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - SUBMENU_HANDLE) / 4;
    if(handle < SUBMENU_HANDLE || (handle & 3) || index >= MAX_SUBMENUS || !a->submenus[index].native) {
        fault(a, "Invalid ARM submenu handle"); return NULL;
    }
    return &a->submenus[index];
}
static ArmTextBox* get_text_box(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - TEXTBOX_HANDLE) / 4;
    if(handle < TEXTBOX_HANDLE || (handle & 3) || index >= MAX_TEXTBOXES || !a->text_boxes[index].native) {
        fault(a, "Invalid ARM text box handle"); return NULL;
    }
    return &a->text_boxes[index];
}
static ArmVarList* get_var_list(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - VARLIST_HANDLE) / 4;
    if(handle < VARLIST_HANDLE || (handle & 3) || index >= MAX_VARLISTS || !a->var_lists[index].native) {
        fault(a, "Invalid ARM variable item list handle"); return NULL;
    }
    return &a->var_lists[index];
}
static ArmVariableItem* get_item(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - ITEM_HANDLE) / 4;
    if(handle < ITEM_HANDLE || (handle & 3) || index >= MAX_ITEMS || !a->items[index].native) {
        fault(a, "Invalid ARM variable item handle"); return NULL;
    }
    return &a->items[index];
}
static ArmWidget* get_widget(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - WIDGET_HANDLE) / 4;
    if(handle < WIDGET_HANDLE || (handle & 3) || index >= MAX_WIDGETS || !a->widgets[index].native) {
        fault(a, "Invalid ARM widget handle"); return NULL;
    }
    return &a->widgets[index];
}
static ArmTextInput* get_text_input(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - TEXTINPUT_HANDLE) / 4;
    if(handle < TEXTINPUT_HANDLE || (handle & 3) || index >= MAX_TEXT_INPUTS ||
       !a->text_inputs[index].native) {
        fault(a, "Invalid ARM text input handle"); return NULL;
    }
    return &a->text_inputs[index];
}
static ArmDialogMessage* get_dialog_message(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - DIALOG_MESSAGE_HANDLE) / 4;
    if(handle < DIALOG_MESSAGE_HANDLE || (handle & 3) || index >= MAX_DIALOG_MESSAGES ||
       !a->dialog_messages[index].native) {
        fault(a, "Invalid ARM dialog message handle"); return NULL;
    }
    return &a->dialog_messages[index];
}
static ArmDialogEx* get_dialog_ex(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - DIALOGEX_HANDLE) / 4;
    if(handle < DIALOGEX_HANDLE || (handle & 3) || index >= MAX_DIALOG_EX ||
       !a->dialog_exs[index].native) {
        fault(a, "Invalid ARM dialog ex handle"); return NULL;
    }
    return &a->dialog_exs[index];
}
static bool has_scene_manager(ArmFapRuntime* a, uint32_t handle) {
    if(handle == SCENE_MANAGER_HANDLE && a->scene_managers[0].native) return true;
    fault(a, "Invalid ARM scene manager handle"); return false;
}
static bool has_infrared(ArmFapRuntime* a, uint32_t handle) {
    if(handle == INFRARED_HANDLE && a->infrared.native) return true;
    fault(a, "Invalid ARM infrared worker handle"); return false;
}
static uint32_t wrap_nfc_ptr(ArmFapRuntime* a, const void* native, NfcPtrKind kind, bool owned) {
    if(!native) return 0;
    for(unsigned i = 0; i < MAX_NFC_PTRS; i++) if(!a->nfc_ptrs[i].native) {
        a->nfc_ptrs[i].native = native; a->nfc_ptrs[i].kind = kind; a->nfc_ptrs[i].owned = owned;
        return NFC_PTR_HANDLE + i * 4;
    }
    return 0;
}
static ArmNfcPtr* get_nfc_ptr(ArmFapRuntime* a, uint32_t handle, NfcPtrKind kind) {
    uint32_t index = (handle - NFC_PTR_HANDLE) / 4;
    if(handle < NFC_PTR_HANDLE || (handle & 3) || index >= MAX_NFC_PTRS ||
       !a->nfc_ptrs[index].native || a->nfc_ptrs[index].kind != (uint8_t)kind) {
        fault(a, "Invalid ARM NFC handle"); return NULL;
    }
    return &a->nfc_ptrs[index];
}
/* Wrap a View owned by a GUI module (Submenu/Widget/...): the module already
 * wires its own native draw/input context internally, so we never touch
 * view_set_context/draw/input on it -- only the dispatcher add/remove/switch
 * calls need our VIEW_HANDLE bookkeeping. */
static uint32_t wrap_view(ArmFapRuntime* a, View* native) {
    if(!native) return 0;
    for(unsigned i = 0; i < MAX_VIEWS; i++) if(!a->views[i].native) {
        a->views[i] = (ArmView){0};
        a->views[i].owner = a; a->views[i].native = native; a->views[i].external = true;
        return VIEW_HANDLE + i * 4;
    }
    return 0;
}
static ArmPort* get_port(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - PORT_HANDLE) / 4;
    if(handle < PORT_HANDLE || (handle & 3) || index >= MAX_PORTS || !a->ports[index].native) {
        fault(a, "Invalid ARM viewport handle"); return NULL;
    }
    return &a->ports[index];
}
static ArmQueue* get_queue(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - QUEUE_HANDLE) / 4;
    if(handle < QUEUE_HANDLE || (handle & 3) || index >= MAX_QUEUES) return NULL;
    return &a->queues[index];
}
static ArmString* get_string(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - STRING_HANDLE) / 4;
    if(handle < STRING_HANDLE || (handle & 3) || index >= MAX_STRINGS || !a->strings[index].native) {
        fault(a, "Invalid ARM string handle"); return NULL;
    }
    return &a->strings[index];
}
static ArmFile* get_file(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - FILE_HANDLE) / 4;
    if(handle < FILE_HANDLE || (handle & 3) || index >= MAX_FILES || !a->files[index].native) {
        fault(a, "Invalid ARM file handle"); return NULL;
    }
    return &a->files[index];
}
static ArmFormat* get_format(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - FORMAT_HANDLE) / 4;
    if(handle < FORMAT_HANDLE || (handle & 3) || index >= MAX_FORMATS || !a->formats[index].native) {
        fault(a, "Invalid ARM flipper format handle"); return NULL;
    }
    return &a->formats[index];
}
static ArmStream* get_stream(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - STREAM_HANDLE) / 4;
    if(handle < STREAM_HANDLE || (handle & 3) || index >= MAX_STREAMS || !a->streams[index].native) {
        fault(a, "Invalid ARM stream handle"); return NULL;
    }
    return &a->streams[index];
}
static bool has_storage(ArmFapRuntime* a, uint32_t handle) {
    if(handle == STORAGE_HANDLE && a->storage) return true;
    fault(a, "Invalid ARM storage handle"); return false;
}
static uint32_t wrap_format(ArmFapRuntime* a, FlipperFormat* native) {
    if(!native) return 0;
    for(unsigned i = 0; i < MAX_FORMATS; i++) if(!a->formats[i].native) {
        a->formats[i].native = native;
        return FORMAT_HANDLE + i * 4;
    }
    return 0;
}
static uint32_t wrap_stream(ArmFapRuntime* a, Stream* native, bool owned) {
    if(!native) return 0;
    for(unsigned i = 0; i < MAX_STREAMS; i++) if(!a->streams[i].native) {
        a->streams[i].native = native;
        a->streams[i].owned = owned;
        return STREAM_HANDLE + i * 4;
    }
    return 0;
}
static ArmMutex* get_mutex(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - MUTEX_HANDLE) / 4;
    if(handle < MUTEX_HANDLE || (handle & 3) || index >= MAX_MUTEXES || !a->mutexes[index].native) {
        fault(a, "Invalid ARM mutex handle"); return NULL;
    }
    return &a->mutexes[index];
}
static ArmSemaphore* get_semaphore(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - SEMAPHORE_HANDLE) / 4;
    if(handle < SEMAPHORE_HANDLE || (handle & 3) || index >= MAX_SEMAPHORES ||
       !a->semaphores[index].native) {
        fault(a, "Invalid ARM semaphore handle"); return NULL;
    }
    return &a->semaphores[index];
}
static ArmEventFlag* get_event_flag(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - EVENT_FLAG_HANDLE) / 4;
    if(handle < EVENT_FLAG_HANDLE || (handle & 3) || index >= MAX_EVENT_FLAGS ||
       !a->event_flags[index].native) {
        fault(a, "Invalid ARM event flag handle"); return NULL;
    }
    return &a->event_flags[index];
}
static ArmSubGhzDevice* get_subghz_device(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - SUBGHZ_DEVICE_HANDLE) / 4;
    if(handle < SUBGHZ_DEVICE_HANDLE || (handle & 3) || index >= MAX_SUBGHZ_DEVICES ||
       !a->subghz_devices[index].native) {
        fault(a, "Invalid ARM subghz device handle"); return NULL;
    }
    return &a->subghz_devices[index];
}
static ArmSubGhzEnv* get_subghz_env(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - SUBGHZ_ENV_HANDLE) / 4;
    if(handle < SUBGHZ_ENV_HANDLE || (handle & 3) || index >= MAX_SUBGHZ_ENVS ||
       !a->subghz_envs[index].native) {
        fault(a, "Invalid ARM subghz environment handle"); return NULL;
    }
    return &a->subghz_envs[index];
}
static ArmSubGhzSetting* get_subghz_setting(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - SUBGHZ_SETTING_HANDLE) / 4;
    if(handle < SUBGHZ_SETTING_HANDLE || (handle & 3) || index >= MAX_SUBGHZ_SETTINGS ||
       !a->subghz_settings[index].native) {
        fault(a, "Invalid ARM subghz setting handle"); return NULL;
    }
    return &a->subghz_settings[index];
}
static ArmSubGhzReceiver* get_subghz_receiver(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - SUBGHZ_RECEIVER_HANDLE) / 4;
    if(handle < SUBGHZ_RECEIVER_HANDLE || (handle & 3) || index >= MAX_SUBGHZ_RECEIVERS ||
       !a->subghz_receivers[index].native) {
        fault(a, "Invalid ARM subghz receiver handle"); return NULL;
    }
    return &a->subghz_receivers[index];
}
static ArmSubGhzTransmitter* get_subghz_transmitter(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - SUBGHZ_TRANSMITTER_HANDLE) / 4;
    if(handle < SUBGHZ_TRANSMITTER_HANDLE || (handle & 3) || index >= MAX_SUBGHZ_TRANSMITTERS ||
       !a->subghz_transmitters[index].native) {
        fault(a, "Invalid ARM subghz transmitter handle"); return NULL;
    }
    return &a->subghz_transmitters[index];
}
static ArmSubGhzWorker* get_subghz_worker(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - SUBGHZ_WORKER_HANDLE) / 4;
    if(handle < SUBGHZ_WORKER_HANDLE || (handle & 3) || index >= MAX_SUBGHZ_WORKERS ||
       !a->subghz_workers[index].native) {
        fault(a, "Invalid ARM subghz worker handle"); return NULL;
    }
    return &a->subghz_workers[index];
}
static uint32_t wrap_subghz_device(ArmFapRuntime* a, const SubGhzDevice* native) {
    if(!native) return 0;
    for(unsigned i = 0; i < MAX_SUBGHZ_DEVICES; i++) {
        if(a->subghz_devices[i].native == native) return SUBGHZ_DEVICE_HANDLE + i * 4;
    }
    for(unsigned i = 0; i < MAX_SUBGHZ_DEVICES; i++) if(!a->subghz_devices[i].native) {
        a->subghz_devices[i].native = native;
        return SUBGHZ_DEVICE_HANDLE + i * 4;
    }
    return 0;
}
static uint32_t wrap_subghz_env(ArmFapRuntime* a, SubGhzEnvironment* native) {
    if(!native) return 0;
    for(unsigned i = 0; i < MAX_SUBGHZ_ENVS; i++) if(!a->subghz_envs[i].native) {
        a->subghz_envs[i].native = native;
        return SUBGHZ_ENV_HANDLE + i * 4;
    }
    return 0;
}
static uint32_t wrap_subghz_setting(ArmFapRuntime* a, SubGhzSetting* native) {
    if(!native) return 0;
    for(unsigned i = 0; i < MAX_SUBGHZ_SETTINGS; i++) if(!a->subghz_settings[i].native) {
        a->subghz_settings[i].native = native;
        return SUBGHZ_SETTING_HANDLE + i * 4;
    }
    return 0;
}
static uint32_t wrap_subghz_receiver(ArmFapRuntime* a, SubGhzReceiver* native) {
    if(!native) return 0;
    for(unsigned i = 0; i < MAX_SUBGHZ_RECEIVERS; i++) if(!a->subghz_receivers[i].native) {
        a->subghz_receivers[i].native = native;
        return SUBGHZ_RECEIVER_HANDLE + i * 4;
    }
    return 0;
}
static uint32_t wrap_subghz_transmitter(ArmFapRuntime* a, SubGhzTransmitter* native) {
    if(!native) return 0;
    for(unsigned i = 0; i < MAX_SUBGHZ_TRANSMITTERS; i++) if(!a->subghz_transmitters[i].native) {
        a->subghz_transmitters[i].native = native;
        return SUBGHZ_TRANSMITTER_HANDLE + i * 4;
    }
    return 0;
}
static uint32_t subghz_name_handle(ArmFapRuntime* a, const char* name) {
    if(!name) return 0;
    if(!a->subghz_name_buf) a->subghz_name_buf = arm_fap_vm_alloc(&a->vm, SUBGHZ_NAME_BUF_SIZE);
    if(!a->subghz_name_buf) return 0;
    char* dest = arm_fap_vm_pointer(&a->vm, a->subghz_name_buf, SUBGHZ_NAME_BUF_SIZE, true);
    if(!dest) return 0;
    size_t n = strlen(name); if(n > SUBGHZ_NAME_BUF_SIZE - 1) n = SUBGHZ_NAME_BUF_SIZE - 1;
    memcpy(dest, name, n); dest[n] = 0;
    return a->subghz_name_buf;
}
static size_t setting_preset_index(SubGhzSetting* setting, const char* name) {
    size_t count = subghz_setting_get_preset_count(setting);
    for(size_t i = 0; i < count; i++) {
        const char* item = subghz_setting_get_preset_name(setting, i);
        if(item && !strcmp(item, name)) return i;
    }
    return SIZE_MAX;
}
static void subghz_worker_pair_trampoline(void* context, bool level, uint32_t duration) {
    ArmSubGhzWorker* worker = context;
    ArmFapRuntime* a = worker->owner;
    lock(a);
    const uint32_t args[] = {worker->context, level ? 1u : 0u, duration};
    guest_call(a, worker->pair_callback, args, 3, NULL);
    unlock(a);
}
static void subghz_worker_overrun_trampoline(void* context) {
    ArmSubGhzWorker* worker = context;
    ArmFapRuntime* a = worker->owner;
    lock(a);
    guest_call(a, worker->overrun_callback, &worker->context, 1, NULL);
    unlock(a);
}
static uint32_t guest_tick(void) {
    return (uint32_t)((uint64_t)furi_get_tick() * 1000 / furi_kernel_get_tick_frequency());
}
static FuriStatus queue_transfer(ArmFapRuntime* a, ArmQueue* queue, void* data, uint32_t timeout, bool put) {
    /* GUI callbacks must never block on the app they are delivering input to.
     * Poll waits on the app thread so faults/Back can interrupt infinite waits. */
    if(a->gui_callback_depth) timeout = 0;
    uint32_t start = guest_tick(), elapsed = 0;
    FuriStatus status;
    queue->busy++;
    do {
        uint32_t wait = timeout == FuriWaitForever ? 50 : MIN(timeout - elapsed, 50);
        if(!wait) {
            status = put ? furi_message_queue_put(queue->native, data, 0) :
                           furi_message_queue_get(queue->native, data, 0);
        } else {
            unlock(a);
            status = put ? furi_message_queue_put(queue->native, data, furi_ms_to_ticks(wait)) :
                           furi_message_queue_get(queue->native, data, furi_ms_to_ticks(wait));
            lock(a);
        }
        elapsed = guest_tick() - start;
        if(status == FuriStatusOk || status != FuriStatusErrorTimeout || a->vm.error[0]) break;
    } while(timeout == FuriWaitForever || elapsed < timeout);
    queue->busy--;
    if(elapsed) a->vm.yields++;
    return status;
}
/* Poll native waits in short slices while the interpreter mutex is released:
 * GUI callbacks must be able to enter, and a fault/Back must end the wait. */
typedef FuriStatus (*ArmStatusWait)(void* context, uint32_t timeout);
static FuriStatus status_wait(ArmFapRuntime* a, uint32_t timeout, ArmStatusWait fn, void* context) {
    if(a->gui_callback_depth) timeout = 0;
    uint32_t start = guest_tick(), elapsed = 0;
    FuriStatus status;
    do {
        uint32_t wait = timeout == FuriWaitForever ? 50 : MIN(timeout - elapsed, 50);
        if(!wait) {
            status = fn(context, 0);
        } else {
            unlock(a);
            status = fn(context, furi_ms_to_ticks(wait));
            lock(a);
        }
        elapsed = guest_tick() - start;
        if(status == FuriStatusOk || status != FuriStatusErrorTimeout || a->vm.error[0]) break;
    } while(timeout == FuriWaitForever || elapsed < timeout);
    if(elapsed) a->vm.yields++;
    return status;
}
static FuriStatus mutex_acquire_wait(void* context, uint32_t timeout) {
    return furi_mutex_acquire(context, timeout);
}
static FuriStatus semaphore_acquire_wait(void* context, uint32_t timeout) {
    return furi_semaphore_acquire(context, timeout);
}
static uint32_t event_flag_wait_call(
    ArmFapRuntime* a, FuriEventFlag* flag, uint32_t bits, uint32_t options, uint32_t timeout) {
    if(a->gui_callback_depth) timeout = 0;
    uint32_t start = guest_tick(), elapsed = 0, result = 0;
    do {
        uint32_t wait = timeout == FuriWaitForever ? 50 : MIN(timeout - elapsed, 50);
        if(!wait) {
            result = furi_event_flag_wait(flag, bits, options, 0);
        } else {
            unlock(a);
            result = furi_event_flag_wait(flag, bits, options, furi_ms_to_ticks(wait));
            lock(a);
        }
        elapsed = guest_tick() - start;
        bool matched = (options & FuriFlagWaitAll) ? ((result & bits) == bits) :
                                                    ((result & bits) != 0);
        if(matched || a->vm.error[0]) break;
    } while(timeout == FuriWaitForever || elapsed < timeout);
    if(elapsed) a->vm.yields++;
    return result;
}
static bool speaker_acquire_wait(ArmFapRuntime* a, uint32_t timeout) {
    if(a->gui_callback_depth) timeout = 0;
    uint32_t start = guest_tick(), elapsed = 0;
    bool acquired = false;
    do {
        uint32_t wait = timeout == FuriWaitForever ? 50 : MIN(timeout - elapsed, 50);
        if(!wait) {
            acquired = furi_hal_speaker_acquire(0);
        } else {
            unlock(a);
            acquired = furi_hal_speaker_acquire(furi_ms_to_ticks(wait));
            lock(a);
        }
        elapsed = guest_tick() - start;
        if(acquired || a->vm.error[0]) break;
    } while(timeout == FuriWaitForever || elapsed < timeout);
    if(elapsed) a->vm.yields++;
    return acquired;
}
static uint32_t log_next_arg(ArmFapVm* vm, uint32_t* index) {
    uint32_t i = (*index)++;
    if(i == 0) return vm->r[3];
    return arm_fap_vm_read(vm, vm->r[13] + (i - 1) * 4, 4);
}
static uint64_t log_next_arg64(ArmFapVm* vm, uint32_t* index) {
    if(*index & 1) (*index)++; /* AAPCS: 64-bit variadic arguments are 8-byte aligned. */
    uint32_t low = log_next_arg(vm, index);
    uint32_t high = log_next_arg(vm, index);
    return (uint64_t)low | ((uint64_t)high << 32);
}
static void log_guest_message(ArmFapRuntime* a, FuriLogLevel level, const char* tag, const char* format) {
    ArmFapVm* vm = &a->vm;
    char output[LOG_BUF_SIZE];
    size_t used = 0;
    uint32_t index = 0;
    const char* cursor = format;
    output[0] = 0;
    while(*cursor && used < LOG_BUF_SIZE - 1 && !vm->error[0]) {
        if(*cursor != '%') { output[used++] = *cursor++; output[used] = 0; continue; }
        if(cursor[1] == '%') { output[used++] = '%'; output[used] = 0; cursor += 2; continue; }
        char spec[16];
        size_t n = 0;
        spec[n++] = *cursor++;
        while(n < sizeof(spec) - 2 && *cursor && strchr("-+ #0.123456789lh", *cursor))
            spec[n++] = *cursor++;
        char conversion = *cursor;
        if(!conversion) break;
        cursor++;
        spec[n++] = conversion;
        spec[n] = 0;
        if(conversion == 's' || conversion == 'c' || conversion == 'd' || conversion == 'i' ||
           conversion == 'u' || conversion == 'x' || conversion == 'X' || conversion == 'o' ||
           conversion == 'p') {
            char value[64];
            int written;
            if(conversion == 's') {
                uint32_t arg = log_next_arg(vm, &index);
                if(vm->error[0]) break;
                const char* string = arg ? arm_fap_vm_string(vm, arg) : NULL;
                if(!string) break;
                written = snprintf(value, sizeof(value), spec, string);
            } else if(conversion == 'c') {
                uint32_t arg = log_next_arg(vm, &index);
                if(vm->error[0]) break;
                written = snprintf(value, sizeof(value), spec, (int)(char)arg);
            } else if(strstr(spec, "ll")) {
                uint64_t arg = log_next_arg64(vm, &index);
                if(vm->error[0]) break;
                if(conversion == 'd' || conversion == 'i')
                    written = snprintf(value, sizeof(value), spec, (long long)arg);
                else
                    written = snprintf(value, sizeof(value), spec, (unsigned long long)arg);
            } else if(conversion == 'd' || conversion == 'i') {
                uint32_t arg = log_next_arg(vm, &index);
                if(vm->error[0]) break;
                written = snprintf(value, sizeof(value), spec, (int)arg);
            } else if(conversion == 'p') {
                uint32_t arg = log_next_arg(vm, &index);
                if(vm->error[0]) break;
                written = snprintf(value, sizeof(value), spec, (void*)(uintptr_t)arg);
            } else {
                uint32_t arg = log_next_arg(vm, &index);
                if(vm->error[0]) break;
                written = snprintf(value, sizeof(value), spec, (unsigned)arg);
            }
            if(written < 0) break;
            size_t chunk = (size_t)written;
            if(chunk > sizeof(value) - 1) chunk = sizeof(value) - 1;
            if(chunk > LOG_BUF_SIZE - 1 - used) chunk = LOG_BUF_SIZE - 1 - used;
            memcpy(output + used, value, chunk);
            used += chunk;
            output[used] = 0;
        } else {
            size_t chunk = n;
            if(chunk > LOG_BUF_SIZE - 1 - used) chunk = LOG_BUF_SIZE - 1 - used;
            memcpy(output + used, spec, chunk);
            used += chunk;
            output[used] = 0;
        }
    }
    furi_log_print_format(level, tag, "%s", output);
}
static bool draw_icon(ArmFapRuntime* a, int32_t x, int32_t y, uint32_t icon) {
    ArmFapVm* vm = &a->vm;
    if(!arm_fap_vm_pointer(vm, icon, 12, false)) return false;
    uint32_t w = arm_fap_vm_read(vm, icon, 2), h = arm_fap_vm_read(vm, icon + 2, 2);
    uint32_t frames = arm_fap_vm_read(vm, icon + 8, 4);
    if(!w || w > 128 || !h || h > 64 || arm_fap_vm_read(vm, icon + 4, 1) != 1) return false;
    uint32_t data = arm_fap_vm_read(vm, frames, 4);
    uint32_t compressed = arm_fap_vm_read(vm, data, 1);
    size_t bytes = ((w + 7) / 8) * h;
    const uint8_t* bitmap = NULL;
    if(compressed == 0) bitmap = arm_fap_vm_pointer(vm, data + 1, bytes, false);
    else if(compressed == 1) {
        uint32_t n = arm_fap_vm_read(vm, data + 2, 2);
        if(!n || n > 2048) return false;
        uint8_t* source = arm_fap_vm_pointer(vm, data, n + 4, false);
        if(!source) return false;
        if(!a->compress) a->compress = compress_alloc(CompressTypeHeatshrink, &compress_config_heatshrink_default);
        size_t decoded = 0;
        if(!compress_decode(a->compress, source, n + 4, a->bitmap, sizeof(a->bitmap), &decoded) || decoded != bytes) return false;
        bitmap = a->bitmap;
    }
    if(!bitmap || vm->error[0]) return false;
    canvas_draw_xbm(a->canvas, x, y, w, h, bitmap);
    return true;
}
static ArmPopup* get_popup(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - POPUP_HANDLE) / 4;
    if(handle < POPUP_HANDLE || (handle & 3) || index >= MAX_OTHER_MODULES || !a->popups[index].native) {
        fault(a, "Invalid ARM popup handle"); return NULL;
    }
    return &a->popups[index];
}
static ArmLoading* get_loading(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - LOADING_HANDLE) / 4;
    if(handle < LOADING_HANDLE || (handle & 3) || index >= MAX_OTHER_MODULES || !a->loadings[index].native) {
        fault(a, "Invalid ARM loading handle"); return NULL;
    }
    return &a->loadings[index];
}
static ArmNumberInput* get_number_input(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - NUMBER_INPUT_HANDLE) / 4;
    if(handle < NUMBER_INPUT_HANDLE || (handle & 3) || index >= MAX_OTHER_MODULES || !a->number_inputs[index].native) {
        fault(a, "Invalid ARM number_input handle"); return NULL;
    }
    return &a->number_inputs[index];
}
static ArmByteInput* get_byte_input(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - BYTE_INPUT_HANDLE) / 4;
    if(handle < BYTE_INPUT_HANDLE || (handle & 3) || index >= MAX_OTHER_MODULES || !a->byte_inputs[index].native) {
        fault(a, "Invalid ARM byte_input handle"); return NULL;
    }
    return &a->byte_inputs[index];
}
static ArmMenu* get_menu(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - MENU_HANDLE) / 4;
    if(handle < MENU_HANDLE || (handle & 3) || index >= MAX_OTHER_MODULES || !a->menus[index].native) {
        fault(a, "Invalid ARM menu handle"); return NULL;
    }
    return &a->menus[index];
}
static void popup_trampoline(void* context) {
    ArmPopup* module = context; ArmFapRuntime* a = module->owner;
    lock(a); a->gui_callback_depth++;
    if(module->callback) guest_call(a, module->callback, &module->context, 1, NULL);
    a->gui_callback_depth--; unlock(a);
}
static void number_input_trampoline(void* context, int32_t value) {
    ArmNumberInput* module = context; ArmFapRuntime* a = module->owner;
    lock(a); a->gui_callback_depth++;
    uint32_t args[] = {module->context, (uint32_t)value};
    if(module->callback) guest_call(a, module->callback, args, 2, NULL);
    a->gui_callback_depth--; unlock(a);
}
static void byte_input_deliver(ArmByteInput* module, bool changed) {
    ArmFapRuntime* a = module->owner;
    lock(a); a->gui_callback_depth++;
    void* dst = arm_fap_vm_pointer(&a->vm, module->buffer, module->count, true);
    if(dst) {
        memcpy(dst, module->bytes, module->count);
        uint32_t callback = changed ? module->changed : module->callback;
        if(callback) guest_call(a, callback, &module->context, 1, NULL);
        memcpy(module->bytes, dst, module->count);
    } else fault(a, "Invalid ARM byte input buffer");
    a->gui_callback_depth--; unlock(a);
}
static void byte_input_trampoline(void* context) { byte_input_deliver(context, false); }
static void byte_changed_trampoline(void* context) { byte_input_deliver(context, true); }
static void menu_trampoline(void* context, uint32_t index) {
    ArmMenuEntry* entry = context; ArmFapRuntime* a = entry->owner;
    lock(a); a->gui_callback_depth++;
    uint32_t args[] = {entry->context, index};
    if(entry->callback) guest_call(a, entry->callback, args, 2, NULL);
    a->gui_callback_depth--; unlock(a);
}

static ArmBitBuffer* get_bit_buffer(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - BIT_BUFFER_HANDLE) / 4;
    if(handle < BIT_BUFFER_HANDLE || (handle & 3) || index >= MAX_BIT_BUFFERS || !a->bit_buffers[index].native) {
        fault(a, "Invalid ARM bit_buffer handle"); return NULL;
    }
    return &a->bit_buffers[index];
}
static ArmDirWalk* get_dir_walk(ArmFapRuntime* a, uint32_t handle) {
    uint32_t index = (handle - DIR_WALK_HANDLE) / 4;
    if(handle < DIR_WALK_HANDLE || (handle & 3) || index >= MAX_DIR_WALKS || !a->dir_walks[index].native) {
        fault(a, "Invalid ARM dir_walk handle"); return NULL;
    }
    return &a->dir_walks[index];
}

/* DateTime has five byte fields, a padded uint16 year, and a weekday. */
static bool guest_datetime(ArmFapVm* vm, uint32_t address, DateTime* date) {
    if(!arm_fap_vm_pointer(vm, address, 10, false)) return false;
    *date = (DateTime){.hour = arm_fap_vm_read(vm, address, 1),
        .minute = arm_fap_vm_read(vm, address + 1, 1), .second = arm_fap_vm_read(vm, address + 2, 1),
        .day = arm_fap_vm_read(vm, address + 3, 1), .month = arm_fap_vm_read(vm, address + 4, 1),
        .year = arm_fap_vm_read(vm, address + 6, 2), .weekday = arm_fap_vm_read(vm, address + 8, 1)};
    if(date->hour > 23 || date->minute > 59 || date->second > 59 || date->month < 1 || date->month > 12 ||
       date->year < 1970 || date->year > 2106 || date->day < 1 || date->weekday < 1 || date->weekday > 7 ||
       date->day > datetime_get_days_per_month(datetime_is_leap_year(date->year), date->month)) {
        arm_fap_vm_fault(vm, "Invalid ARM date/time"); return false;
    }
    return true;
}

/* Bounded string operations may stop before a NUL and must not scan beyond n. */
static bool guest_string_span(ArmFapVm* vm, uint32_t address, uint32_t n, uint32_t* span) {
    if(n > ARM_FAP_RAM_SIZE) { arm_fap_vm_fault(vm, "Guest string size exceeds arena"); return false; }
    for(uint32_t i = 0; i < n; i++) {
        const uint8_t* byte = arm_fap_vm_pointer(vm, address + i, 1, false);
        if(!byte) return false;
        if(!*byte) { *span = i + 1; return true; }
    }
    *span = n; return true;
}
static bool import_call(ArmFapVm* vm, ArmFapImport id, void* context) {
    ArmFapRuntime* a = context;
    uint32_t p = vm->r[0], q = vm->r[1], r = vm->r[2], s = vm->r[3], result = 0;
    ArmPopup* popup = NULL;
    if(id >= ArmImport_popup_alloc && id <= ArmImport_popup_disable_timeout) {
        if(a->gui_callback_depth) { fault(a, "Module operation in GUI callback"); return false; }
        if(id != ArmImport_popup_alloc) { popup = get_popup(a, p); if(!popup) return false; }
    }
    ArmLoading* loading = NULL;
    if(id >= ArmImport_loading_alloc && id <= ArmImport_loading_get_view) {
        if(a->gui_callback_depth) { fault(a, "Module operation in GUI callback"); return false; }
        if(id != ArmImport_loading_alloc) { loading = get_loading(a, p); if(!loading) return false; }
    }
    ArmNumberInput* number_input = NULL;
    if(id >= ArmImport_number_input_alloc && id <= ArmImport_number_input_set_result_callback) {
        if(a->gui_callback_depth) { fault(a, "Module operation in GUI callback"); return false; }
        if(id != ArmImport_number_input_alloc) { number_input = get_number_input(a, p); if(!number_input) return false; }
    }
    ArmByteInput* byte_input = NULL;
    if(id >= ArmImport_byte_input_alloc && id <= ArmImport_byte_input_set_result_callback) {
        if(a->gui_callback_depth) { fault(a, "Module operation in GUI callback"); return false; }
        if(id != ArmImport_byte_input_alloc) { byte_input = get_byte_input(a, p); if(!byte_input) return false; }
    }
    ArmMenu* menu = NULL;
    if(id >= ArmImport_menu_alloc && id <= ArmImport_menu_add_item) {
        if(a->gui_callback_depth) { fault(a, "Module operation in GUI callback"); return false; }
        if(id != ArmImport_menu_alloc) { menu = get_menu(a, p); if(!menu) return false; }
    }
    ArmBitBuffer* bit_buffer = NULL;
    if(id >= ArmImport_bit_buffer_free && id <= ArmImport_bit_buffer_get_size_bytes) {
        bit_buffer = get_bit_buffer(a, p); if(!bit_buffer) return false;
    }
    ArmDirWalk* dir_walk = NULL;
    if(id >= ArmImport_dir_walk_free && id <= ArmImport_dir_walk_set_recursive) {
        dir_walk = get_dir_walk(a, p); if(!dir_walk) return false;
    }
    ArmView* v = NULL;
    ArmPort* port = NULL;
    ArmString* str = NULL;
    ArmSubmenu* sub = NULL;
    ArmTextBox* tbox = NULL;
    ArmVarList* list = NULL;
    ArmVariableItem* item = NULL;
    ArmWidget* widget = NULL;
    ArmTextInput* tinput = NULL;
    ArmDialogMessage* dmsg = NULL;
    ArmFile* file = NULL;
    ArmFormat* format = NULL;
    ArmStream* stream = NULL;
    ArmMutex* mutex = NULL;
    ArmSemaphore* semaphore = NULL;
    ArmEventFlag* event_flag = NULL;
    ArmSubGhzDevice* sg_device = NULL;
    ArmSubGhzEnv* sg_env = NULL;
    ArmSubGhzSetting* sg_setting = NULL;
    ArmSubGhzReceiver* sg_receiver = NULL;
    ArmSubGhzTransmitter* sg_transmitter = NULL;
    ArmSubGhzWorker* sg_worker = NULL;
    if(id >= ArmImport_widget_free && id <= ArmImport_widget_add_rect_element) {
        widget = get_widget(a, p); if(!widget) return false;
    }
    if(id >= ArmImport_text_input_free && id <= ArmImport_text_input_set_header_text) {
        tinput = get_text_input(a, p); if(!tinput) return false;
    }
    if(id >= ArmImport_dialog_message_free && id <= ArmImport_dialog_message_set_buttons) {
        dmsg = get_dialog_message(a, p); if(!dmsg) return false;
    }
    ArmDialogEx* dex = NULL;
    if(id >= ArmImport_dialog_ex_free && id <= ArmImport_dialog_ex_set_right_button_text) {
        dex = get_dialog_ex(a, p); if(!dex) return false;
    }
    if(id >= ArmImport_scene_manager_free && id <= ArmImport_scene_manager_stop) {
        if(!has_scene_manager(a, p)) return false;
    }
    if(id >= ArmImport_submenu_free && id <= ArmImport_submenu_set_selected_item) {
        sub = get_submenu(a, p); if(!sub) return false;
    }
    if(id >= ArmImport_text_box_free && id <= ArmImport_text_box_set_focus) {
        tbox = get_text_box(a, p); if(!tbox) return false;
    }
    if(id >= ArmImport_variable_item_list_free && id <= ArmImport_variable_item_list_add) {
        list = get_var_list(a, p); if(!list) return false;
    }
    if(id >= ArmImport_variable_item_get_context &&
       id <= ArmImport_variable_item_set_current_value_text) {
        item = get_item(a, p); if(!item) return false;
    }
    if(id >= ArmImport_view_free && id <= ArmImport_view_set_orientation) {
        v = get_view(a, p); if(!v) return false;
        if(v->external) { fault(a, "View is owned by a GUI module"); return false; }
    }
    if(id >= ArmImport_view_dispatcher_free && id <= ArmImport_view_dispatcher_set_custom_event_callback) {
        if(!has_dispatcher(a, p)) return false;
    }
    if(id >= ArmImport_view_holder_free && id <= ArmImport_view_holder_attach_to_gui) {
        if(!has_holder(a, p)) return false;
    }
    if((id >= ArmImport_canvas_clear && id <= ArmImport_elements_text_box) ||
       (id >= ArmImport_canvas_draw_box && id <= ArmImport_canvas_draw_str)) {
        if(p != CANVAS_HANDLE || !a->canvas) { fault(a, "Canvas used outside draw callback"); return false; }
        if(id >= ArmImport_canvas_draw_icon &&
           ((int32_t)q < -1024 || (int32_t)q > 1024 || (int32_t)r < -1024 || (int32_t)r > 1024)) return false;
    }
    if(id >= ArmImport_furi_string_free && id <= ArmImport_furi_string_utf8_length) {
        str = get_string(a, p); if(!str) return false;
    }
    if(id >= ArmImport_storage_file_free && id <= ArmImport_storage_dir_read) {
        file = get_file(a, p); if(!file) return false;
    }
    if(id >= ArmImport_storage_file_exists && id <= ArmImport_storage_simply_remove_recursive) {
        if(!has_storage(a, p)) return false;
    }
    if(id >= ArmImport_flipper_format_free && id <= ArmImport_flipper_format_insert_or_update_uint32) {
        format = get_format(a, p); if(!format) return false;
    }
    if(id >= ArmImport_stream_clean && id <= ArmImport_stream_write_char) {
        stream = get_stream(a, p); if(!stream) return false;
    }
    if(id >= ArmImport_furi_mutex_free && id <= ArmImport_furi_mutex_release) {
        mutex = get_mutex(a, p); if(!mutex) return false;
    }
    if(id >= ArmImport_furi_semaphore_free && id <= ArmImport_furi_semaphore_release) {
        semaphore = get_semaphore(a, p); if(!semaphore) return false;
    }
    if(id >= ArmImport_furi_event_flag_free && id <= ArmImport_furi_event_flag_wait) {
        event_flag = get_event_flag(a, p); if(!event_flag) return false;
    }
    if(id >= ArmImport_subghz_devices_begin && id <= ArmImport_subghz_devices_sleep) {
        sg_device = get_subghz_device(a, p); if(!sg_device) return false;
    }
    if(id >= ArmImport_subghz_environment_free && id <= ArmImport_subghz_environment_load_keystore) {
        sg_env = get_subghz_env(a, p); if(!sg_env) return false;
    }
    if(id >= ArmImport_subghz_setting_free && id <= ArmImport_subghz_setting_load_custom_preset) {
        sg_setting = get_subghz_setting(a, p); if(!sg_setting) return false;
    }
    if(id >= ArmImport_subghz_receiver_free && id <= ArmImport_subghz_receiver_set_filter) {
        sg_receiver = get_subghz_receiver(a, p); if(!sg_receiver) return false;
    }
    if(id >= ArmImport_subghz_transmitter_deserialize && id <= ArmImport_subghz_transmitter_yield) {
        sg_transmitter = get_subghz_transmitter(a, p); if(!sg_transmitter) return false;
    }
    if(id >= ArmImport_subghz_worker_free && id <= ArmImport_subghz_worker_stop) {
        sg_worker = get_subghz_worker(a, p); if(!sg_worker) return false;
    }
    switch(id) {
    case ArmImport_furi_get_tick: result = guest_tick(); break;
    case ArmImport_furi_ms_to_ticks: result = p; break; /* Flipper guest ticks are milliseconds. */
    case ArmImport_furi_delay_ms: {
        if(p > 60000) return false;
        uint32_t start = guest_tick(), left = p;
        do {
            GUI_CALL(furi_delay_ms(MIN(left, 50)));
            uint32_t elapsed = guest_tick() - start;
            left = elapsed >= p ? 0 : p - elapsed;
        } while(left && !vm->error[0]);
        if(guest_tick() != start) vm->yields++;
        break;
    }
    case ArmImport_rand: result = furi_hal_random_get() & 0x7fffffffu; break;
    case ArmImport_furi_message_queue_alloc:
        if(!p || !q || p > 32 || q > 256 || p * q > 4096) return false;
        for(unsigned i = 0; i < MAX_QUEUES; i++) if(!a->queues[i].native) {
            a->queues[i].native = furi_message_queue_alloc(p, q);
            a->queues[i].size = q;
            result = QUEUE_HANDLE + i * 4; break;
        }
        break;
    case ArmImport_furi_message_queue_free: {
        ArmQueue* queue = get_queue(a, p);
        if(!queue || !queue->native || queue->busy) return false;
        furi_message_queue_free(queue->native); queue->native = NULL; break;
    }
    case ArmImport_furi_message_queue_reset: {
        ArmQueue* queue = get_queue(a, p);
        if(!queue || !queue->native) { result = (uint32_t)FuriStatusErrorParameter; break; }
        result = (uint32_t)furi_message_queue_reset(queue->native); break;
    }
    case ArmImport_furi_message_queue_get:
    case ArmImport_furi_message_queue_put: {
        ArmQueue* queue = get_queue(a, p);
        /* Late release events may reach an app's callback after it frees its
         * input queue, but before it removes its viewport. Never use freed RAM. */
        if(!queue || !queue->native) { result = (uint32_t)FuriStatusErrorParameter; break; }
        bool put = id == ArmImport_furi_message_queue_put;
        void* data = arm_fap_vm_pointer(vm, q, queue->size, !put);
        if(!data) return false;
        result = (uint32_t)queue_transfer(a, queue, data, r, put); break;
    }
    case ArmImport_view_port_alloc:
        for(unsigned i = 0; i < MAX_PORTS; i++) if(!a->ports[i].native) {
            port = &a->ports[i]; port->owner = a; port->native = view_port_alloc();
            result = PORT_HANDLE + i * 4; break;
        }
        break;
    case ArmImport_view_port_free:
    case ArmImport_view_port_draw_callback_set:
    case ArmImport_view_port_input_callback_set:
    case ArmImport_view_port_update:
    case ArmImport_view_port_enabled_set:
    case ArmImport_view_port_set_orientation:
        port = get_port(a, p); if(!port) return false;
        if(id == ArmImport_view_port_free) {
            if(port->added) return false;
            GUI_CALL(view_port_free(port->native)); memset(port, 0, sizeof(*port));
        } else if(id == ArmImport_view_port_draw_callback_set) {
            port->draw = q; port->draw_context = r;
            GUI_CALL(view_port_draw_callback_set(port->native, q ? port_draw_callback : NULL, port));
        } else if(id == ArmImport_view_port_input_callback_set) {
            port->input = q; port->input_context = r;
            GUI_CALL(view_port_input_callback_set(port->native, q ? port_input_callback : NULL, port));
        } else if(id == ArmImport_view_port_update) {
            if(a->gui_callback_depth) view_port_update(port->native);
            else { GUI_CALL(view_port_update(port->native)); }
        } else if(id == ArmImport_view_port_set_orientation) {
            if(q >= ViewPortOrientationMAX) return false;
            GUI_CALL(view_port_set_orientation(port->native, (ViewPortOrientation)q));
        } else {
            GUI_CALL(view_port_enabled_set(port->native, q != 0));
        }
        break;
    case ArmImport_gui_add_view_port:
    case ArmImport_gui_remove_view_port:
        if(p != GUI_HANDLE || !a->gui_refs) return false;
        port = get_port(a, q); if(!port) return false;
        if(id == ArmImport_gui_add_view_port) {
            if(port->added || (r != GuiLayerFullscreen && r != GuiLayerWindow)) return false;
            GUI_CALL(gui_add_view_port(a->gui, port->native, (GuiLayer)r));
            port->added = true;
        } else {
            if(!port->added) return false;
            GUI_CALL(gui_remove_view_port(a->gui, port->native)); port->added = false;
        }
        break;
    case ArmImport_gui_direct_draw_acquire:
        if(p != GUI_HANDLE || !a->gui_refs || a->canvas || a->gui_callback_depth) return false;
        GUI_CALL(a->canvas = gui_direct_draw_acquire(a->gui));
        a->direct_draw = true;
        result = CANVAS_HANDLE;
        break;
    case ArmImport_gui_direct_draw_release:
        if(p != GUI_HANDLE || !a->direct_draw) return false;
        GUI_CALL(gui_direct_draw_release(a->gui));
        a->canvas = NULL; a->direct_draw = false;
        break;
    case ArmImport_notification_message:
    case ArmImport_notification_message_block: {
        if(p != NOTIFICATION_HANDLE || !a->notification_refs) return false;
        const NotificationSequence* sequence;
        if(q == ARM_FAP_IMPORT_BASE + ArmImport_sequence_display_backlight_enforce_on * 4 + 1) {
            sequence = &sequence_display_backlight_enforce_on; a->backlight_forced = true;
        } else if(q == ARM_FAP_IMPORT_BASE + ArmImport_sequence_display_backlight_enforce_auto * 4 + 1) {
            sequence = &sequence_display_backlight_enforce_auto; a->backlight_forced = false;
        } else { fault(a, "Unsupported ARM notification sequence"); return false; }
        if(id == ArmImport_notification_message_block) { GUI_CALL(notification_message_block(a->notification, sequence)); }
        else { GUI_CALL(notification_message(a->notification, sequence)); }
        break;
    }
    /* Other-library libc bridges: all pointer results remain guest addresses. */
    case ArmImport_abort:
    case ArmImport___assert_func: fault(a, "ARM app called abort/assert"); return false;
    case ArmImport___errno:
        if(!a->errno_buf) a->errno_buf = arm_fap_vm_alloc(vm, 4);
        result = a->errno_buf; break;
    case ArmImport_calloc:
        if(p && q <= ARM_FAP_RAM_SIZE / p) result = arm_fap_vm_alloc(vm, p * q);
        break;
    case ArmImport_realloc: {
        if(!p) { result = arm_fap_vm_alloc(vm, q); break; }
        unsigned i;
        for(i = 0; i < 32; i++)
            if(vm->allocations[i].used && vm->allocations[i].address == p) break;
        if(i == 32) { fault(a, "Invalid guest realloc"); return false; }
        if(!q) { if(!arm_fap_vm_free(vm, p)) return false; break; }
        if(q <= vm->allocations[i].size) { result = p; break; }
        result = arm_fap_vm_alloc(vm, q);
        if(result) {
            void* dst = arm_fap_vm_pointer(vm, result, q, true);
            const void* src = arm_fap_vm_pointer(vm, p, vm->allocations[i].size, false);
            if(!dst || !src) return false;
            memcpy(dst, src, vm->allocations[i].size);
            if(!arm_fap_vm_free(vm, p)) return false;
        }
        break;
    }
    case ArmImport_memchr:
    case ArmImport_memcmp: {
        if(!r) break;
        const void* left = arm_fap_vm_pointer(vm, p, r, false);
        if(!left) return false;
        if(id == ArmImport_memchr) {
            const char* found = memchr(left, q, r);
            result = found ? p + (uint32_t)(found - (const char*)left) : 0;
        } else {
            const void* right = arm_fap_vm_pointer(vm, q, r, false);
            if(!right) return false;
            result = memcmp(left, right, r);
        }
        break;
    }
    case ArmImport_strncmp:
    case ArmImport_strncasecmp: {
        if(!r) break;
        uint32_t lspan, rspan;
        if(!guest_string_span(vm, p, r, &lspan) || !guest_string_span(vm, q, r, &rspan)) return false;
        const char* left = arm_fap_vm_pointer(vm, p, lspan, false);
        const char* right = arm_fap_vm_pointer(vm, q, rspan, false);
        if(!left || !right) return false;
        result = id == ArmImport_strncmp ? strncmp(left, right, r) : strncasecmp(left, right, r);
        break;
    }
    case ArmImport_strncpy:
    case ArmImport_strcpy: {
        result = p;
        if(id == ArmImport_strncpy && !r) break;
        uint32_t span;
        const char* src;
        if(id == ArmImport_strcpy) {
            src = arm_fap_vm_string(vm, q); if(!src) return false;
            span = strlen(src) + 1; r = span;
        } else {
            if(!guest_string_span(vm, q, r, &span)) return false;
            src = arm_fap_vm_pointer(vm, q, span, false);
        }
        char* dst = arm_fap_vm_pointer(vm, p, r, true);
        if(!src || !dst) return false;
        if(p < q + span && q < p + r) { fault(a, "Overlapping guest string copy"); return false; }
        memcpy(dst, src, span);
        if(span < r) memset(dst + span, 0, r - span);
        break;
    }
    case ArmImport_strcasecmp:
    case ArmImport_strchr:
    case ArmImport_strrchr:
    case ArmImport_strstr:
    case ArmImport_strdup: {
        const char* left = arm_fap_vm_string(vm, p);
        if(!left) return false;
        const char* found = NULL;
        if(id == ArmImport_strdup) {
            size_t size = strlen(left) + 1;
            result = arm_fap_vm_alloc(vm, size);
            if(result) {
                void* dst = arm_fap_vm_pointer(vm, result, size, true);
                if(!dst) return false;
                memcpy(dst, left, size);
            }
            break;
        }
        if(id == ArmImport_strchr) found = strchr(left, q);
        else if(id == ArmImport_strrchr) found = strrchr(left, q);
        else {
            const char* right = arm_fap_vm_string(vm, q);
            if(!right) return false;
            if(id == ArmImport_strcasecmp) { result = strcasecmp(left, right); break; }
            found = strstr(left, right);
        }
        result = found ? p + (uint32_t)(found - left) : 0;
        break;
    }
    case ArmImport_atoi:
    case ArmImport_strtof:
    case ArmImport_strtol:
    case ArmImport_strtoul:
    case ArmImport_strtoull: {
        const char* input = arm_fap_vm_string(vm, p);
        if(!input) return false;
        if(id == ArmImport_atoi) { result = atoi(input); break; }
        if(q && !arm_fap_vm_pointer(vm, q, 4, true)) return false;
        if(id != ArmImport_strtof && r && (r < 2 || r > 36)) {
            fault(a, "Invalid integer base"); return false;
        }
        char* end = NULL;
        int saved_errno = errno;
        errno = a->errno_buf ? (int)arm_fap_vm_read(vm, a->errno_buf, 4) : 0;
        if(id == ArmImport_strtof) { float value = strtof(input, &end); memcpy(&result, &value, 4); }
        else if(id == ArmImport_strtol) result = strtol(input, &end, r);
        else if(id == ArmImport_strtoul) result = strtoul(input, &end, r);
        else {
            uint64_t value = strtoull(input, &end, r);
            result = value; vm->r[1] = value >> 32;
        }
        if(!a->errno_buf) a->errno_buf = arm_fap_vm_alloc(vm, 4);
        if(a->errno_buf) arm_fap_vm_write(vm, a->errno_buf, errno, 4);
        errno = saved_errno;
        if(q && !arm_fap_vm_write(vm, q, p + (uint32_t)(end - input), 4)) return false;
        break;
    }
    case ArmImport_random: result = random(); break;
    case ArmImport_srand: srand(p); break;
    case ArmImport_roundf: {
        float value; memcpy(&value, &p, 4); value = roundf(value); memcpy(&result, &value, 4); break;
    }
    case ArmImport_strint_to_uint32: {
        const char* input = arm_fap_vm_string(vm, p);
        if(!input || !arm_fap_vm_pointer(vm, r, 4, true) ||
           (q && !arm_fap_vm_pointer(vm, q, 4, true))) return false;
        if(s && (s < 2 || s > 36)) { fault(a, "Invalid integer base"); return false; }
        uint32_t value; char* end = NULL;
        result = strint_to_uint32(input, &end, &value, s);
        if(result == StrintParseNoError) {
            if(!arm_fap_vm_write(vm, r, value, 4)) return false;
            if(q && !arm_fap_vm_write(vm, q, p + (uint32_t)(end - input), 4)) return false;
        }
        break;
    }
    case ArmImport_value_index_uint32:
        if(r > ARM_FAP_RAM_SIZE / 4 || (r && !arm_fap_vm_pointer(vm, q, r * 4, false))) return false;
        for(uint32_t i = 0; i < r; i++)
            if(arm_fap_vm_read(vm, q + i * 4, 4) == p) { result = i; break; }
        break;
    case ArmImport_hex_char_to_uint8: {
        uint8_t* output = arm_fap_vm_pointer(vm, r, 1, true);
        if(!output) return false;
        result = hex_char_to_uint8(p, q, output); break;
    }
    case ArmImport_bit_lib_bytes_to_num_bcd:
    case ArmImport_bit_lib_bytes_to_num_be:
    case ArmImport_bit_lib_bytes_to_num_le: {
        bool bcd = id == ArmImport_bit_lib_bytes_to_num_bcd;
        if(q > (bcd ? 9u : 8u)) { fault(a, "Invalid bit conversion width"); return false; }
        uint8_t empty = 0;
        const uint8_t* bytes = q ? arm_fap_vm_pointer(vm, p, q, false) : &empty;
        if(!bytes || (bcd && !arm_fap_vm_pointer(vm, r, 1, true))) return false;
        bool valid = false;
        uint64_t value = bcd ? bit_lib_bytes_to_num_bcd(bytes, q, &valid) :
            id == ArmImport_bit_lib_bytes_to_num_be ? bit_lib_bytes_to_num_be(bytes, q) :
            bit_lib_bytes_to_num_le(bytes, q);
        if(bcd && !arm_fap_vm_write(vm, r, valid, 1)) return false;
        result = value; vm->r[1] = value >> 32; break;
    }
    case ArmImport_bit_lib_num_to_bytes_be: {
        if(r > 8) { fault(a, "Invalid bit conversion width"); return false; }
        if(!r) break;
        uint8_t* output = arm_fap_vm_pointer(vm, s, r, true);
        if(!output) return false;
        bit_lib_num_to_bytes_be((uint64_t)p | ((uint64_t)q << 32), r, output); break;
    }
    case ArmImport_bit_lib_get_bits:
    case ArmImport_bit_lib_get_bits_16:
    case ArmImport_bit_lib_get_bits_32:
    case ArmImport_bit_lib_get_bits_64: {
        uint32_t width = id == ArmImport_bit_lib_get_bits ? 8 :
            id == ArmImport_bit_lib_get_bits_16 ? 16 : id == ArmImport_bit_lib_get_bits_32 ? 32 : 64;
        if(!r || r > width || q > ARM_FAP_RAM_SIZE * 8 - r) {
            fault(a, "Invalid bit range"); return false;
        }
        const uint8_t* bytes = arm_fap_vm_pointer(vm, p, (q + r + 7) / 8, false);
        if(!bytes) return false;
        /* The native helper overreads one byte on unaligned fields. Read only
         * the requested bits here, including the final byte at an arena edge. */
        uint64_t value = 0;
        for(uint32_t i = 0; i < r; i++) value = (value << 1) | ((bytes[(q+i)/8] >> (7-(q+i)%8)) & 1);
        result = value;
        if(width == 64) vm->r[1] = value >> 32;
        break;
    }
    case ArmImport_popup_alloc:
        for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(!a->popups[i].native) {
            ArmPopup* module = &a->popups[i]; module->owner = a;
            GUI_CALL(module->native = popup_alloc());
            if(module->native) result = POPUP_HANDLE + i * 4;
            break;
        }
        break;
    case ArmImport_popup_free: {
        ArmPopup* module = popup;
        ArmView* view = module->view_handle ? get_view(a, module->view_handle) : NULL;
        if(view && (view->added || a->holder.native)) { fault(a, "Module view still attached"); return false; }
        GUI_CALL(popup_free(module->native));
        if(view) memset(view, 0, sizeof(*view));
        free(module->header); free(module->text);
        memset(module, 0, sizeof(*module)); break;
    }
    case ArmImport_popup_get_view:
        if(!popup->view_handle) popup->view_handle = wrap_view(a, popup_get_view(popup->native));
        if(!popup->view_handle) return false;
        result = popup->view_handle; break;
    case ArmImport_loading_alloc:
        for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(!a->loadings[i].native) {
            ArmLoading* module = &a->loadings[i]; module->owner = a;
            GUI_CALL(module->native = loading_alloc());
            if(module->native) result = LOADING_HANDLE + i * 4;
            break;
        }
        break;
    case ArmImport_loading_free: {
        ArmLoading* module = loading;
        ArmView* view = module->view_handle ? get_view(a, module->view_handle) : NULL;
        if(view && (view->added || a->holder.native)) { fault(a, "Module view still attached"); return false; }
        GUI_CALL(loading_free(module->native));
        if(view) memset(view, 0, sizeof(*view));
        memset(module, 0, sizeof(*module)); break;
    }
    case ArmImport_loading_get_view:
        if(!loading->view_handle) loading->view_handle = wrap_view(a, loading_get_view(loading->native));
        if(!loading->view_handle) return false;
        result = loading->view_handle; break;
    case ArmImport_number_input_alloc:
        for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(!a->number_inputs[i].native) {
            ArmNumberInput* module = &a->number_inputs[i]; module->owner = a;
            GUI_CALL(module->native = number_input_alloc());
            if(module->native) result = NUMBER_INPUT_HANDLE + i * 4;
            break;
        }
        break;
    case ArmImport_number_input_free: {
        ArmNumberInput* module = number_input;
        ArmView* view = module->view_handle ? get_view(a, module->view_handle) : NULL;
        if(view && (view->added || a->holder.native)) { fault(a, "Module view still attached"); return false; }
        GUI_CALL(number_input_free(module->native));
        if(view) memset(view, 0, sizeof(*view));
        free(module->header);
        memset(module, 0, sizeof(*module)); break;
    }
    case ArmImport_number_input_get_view:
        if(!number_input->view_handle) number_input->view_handle = wrap_view(a, number_input_get_view(number_input->native));
        if(!number_input->view_handle) return false;
        result = number_input->view_handle; break;
    case ArmImport_number_input_set_header_text: {
        const char* text = arm_fap_vm_string(vm, q); if(!text) return false;
        char* copy = strdup(text); if(!copy) return false;
        GUI_CALL(number_input_set_header_text(number_input->native, copy));
        free(number_input->header); number_input->header = copy; break;
    }
    case ArmImport_byte_input_alloc:
        for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(!a->byte_inputs[i].native) {
            ArmByteInput* module = &a->byte_inputs[i]; module->owner = a;
            GUI_CALL(module->native = byte_input_alloc());
            if(module->native) result = BYTE_INPUT_HANDLE + i * 4;
            break;
        }
        break;
    case ArmImport_byte_input_free: {
        ArmByteInput* module = byte_input;
        ArmView* view = module->view_handle ? get_view(a, module->view_handle) : NULL;
        if(view && (view->added || a->holder.native)) { fault(a, "Module view still attached"); return false; }
        GUI_CALL(byte_input_free(module->native));
        if(view) memset(view, 0, sizeof(*view));
        free(module->header);
        memset(module, 0, sizeof(*module)); break;
    }
    case ArmImport_byte_input_get_view:
        if(!byte_input->view_handle) byte_input->view_handle = wrap_view(a, byte_input_get_view(byte_input->native));
        if(!byte_input->view_handle) return false;
        result = byte_input->view_handle; break;
    case ArmImport_byte_input_set_header_text: {
        const char* text = arm_fap_vm_string(vm, q); if(!text) return false;
        char* copy = strdup(text); if(!copy) return false;
        GUI_CALL(byte_input_set_header_text(byte_input->native, copy));
        free(byte_input->header); byte_input->header = copy; break;
    }
    case ArmImport_menu_alloc:
        for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(!a->menus[i].native) {
            ArmMenu* module = &a->menus[i]; module->owner = a;
            GUI_CALL(module->native = menu_alloc());
            if(module->native) result = MENU_HANDLE + i * 4;
            break;
        }
        break;
    case ArmImport_menu_free: {
        ArmMenu* module = menu;
        ArmView* view = module->view_handle ? get_view(a, module->view_handle) : NULL;
        if(view && (view->added || a->holder.native)) { fault(a, "Module view still attached"); return false; }
        GUI_CALL(menu_free(module->native));
        if(view) memset(view, 0, sizeof(*view));
        for(unsigned j = 0; j < module->count; j++) free(module->entries[j].label);
        memset(module, 0, sizeof(*module)); break;
    }
    case ArmImport_menu_get_view:
        if(!menu->view_handle) menu->view_handle = wrap_view(a, menu_get_view(menu->native));
        if(!menu->view_handle) return false;
        result = menu->view_handle; break;
    case ArmImport_popup_reset:
        GUI_CALL(popup_reset(popup->native));
        popup->callback = popup->context = 0;
        free(popup->header); free(popup->text); popup->header = popup->text = NULL;
        break;
    case ArmImport_popup_set_callback:
        GUI_CALL(popup_set_callback(popup->native, NULL));
        popup->callback = q;
        GUI_CALL(popup_set_context(popup->native, popup));
        GUI_CALL(popup_set_callback(popup->native, q ? popup_trampoline : NULL)); break;
    case ArmImport_popup_set_context: popup->context = q; break;
    case ArmImport_popup_set_timeout: GUI_CALL(popup_set_timeout(popup->native, q)); break;
    case ArmImport_popup_enable_timeout: GUI_CALL(popup_enable_timeout(popup->native)); break;
    case ArmImport_popup_disable_timeout: GUI_CALL(popup_disable_timeout(popup->native)); break;
    case ArmImport_popup_set_header:
    case ArmImport_popup_set_text: {
        const char* text = q ? arm_fap_vm_string(vm, q) : NULL;
        uint32_t horizontal = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t vertical = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        if((q && !text) || vm->error[0] ||
           (horizontal != AlignLeft && horizontal != AlignRight && horizontal != AlignCenter) ||
           (vertical != AlignTop && vertical != AlignBottom && vertical != AlignCenter)) return false;
        char* copy = text ? strdup(text) : NULL; if(text && !copy) return false;
        if(id == ArmImport_popup_set_header) {
            GUI_CALL(popup_set_header(popup->native, copy, r, s, horizontal, vertical));
            free(popup->header); popup->header = copy;
        } else {
            GUI_CALL(popup_set_text(popup->native, copy, r, s, horizontal, vertical));
            free(popup->text); popup->text = copy;
        }
        break;
    }
    case ArmImport_number_input_set_result_callback: {
        int32_t minimum = arm_fap_vm_read(vm, vm->r[13], 4);
        int32_t maximum = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        if(vm->error[0] || minimum > maximum) return false;
        number_input->callback = q; number_input->context = r;
        GUI_CALL(number_input_set_result_callback(number_input->native,
            q ? number_input_trampoline : NULL, number_input, (int32_t)s, minimum, maximum));
        break;
    }
    case ArmImport_byte_input_set_result_callback: {
        uint32_t buffer = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t count = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        if(vm->error[0] || !count || count > 255) return false;
        const void* bytes = arm_fap_vm_pointer(vm, buffer, count, true); if(!bytes) return false;
        byte_input->callback = q; byte_input->changed = r; byte_input->context = s;
        byte_input->buffer = buffer; byte_input->count = count;
        memcpy(byte_input->bytes, bytes, count);
        GUI_CALL(byte_input_set_result_callback(byte_input->native, byte_input_trampoline,
            byte_changed_trampoline, byte_input, byte_input->bytes, count)); break;
    }
    case ArmImport_menu_reset:
        GUI_CALL(menu_reset(menu->native));
        for(unsigned i = 0; i < menu->count; i++) free(menu->entries[i].label);
        memset(menu->entries, 0, sizeof(menu->entries)); menu->count = 0; break;
    case ArmImport_menu_add_item: {
        if(r) { fault(a, "Persistent guest menu icons are unsupported"); return false; }
        if(menu->count == MAX_ITEMS) return false;
        const char* label = arm_fap_vm_string(vm, q);
        uint32_t callback = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t context = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        if(!label || vm->error[0]) return false;
        ArmMenuEntry* entry = &menu->entries[menu->count];
        entry->label = strdup(label); if(!entry->label) return false;
        entry->owner = a; entry->callback = callback; entry->context = context;
        GUI_CALL(menu_add_item(menu->native, entry->label, NULL, s,
            callback ? menu_trampoline : NULL, entry));
        menu->count++; break;
    }
    case ArmImport_bit_buffer_alloc:
        if(!p || p > ARM_FAP_RAM_SIZE) return false;
        for(unsigned i = 0; i < MAX_BIT_BUFFERS; i++) if(!a->bit_buffers[i].native) {
            ArmBitBuffer* buffer = &a->bit_buffers[i];
            buffer->guest_buf = arm_fap_vm_alloc(vm, p);
            if(!buffer->guest_buf) break;
            buffer->native = bit_buffer_alloc(p); buffer->capacity = p;
            result = BIT_BUFFER_HANDLE + i * 4; break;
        }
        break;
    case ArmImport_bit_buffer_free:
        bit_buffer_free(bit_buffer->native);
        if(!arm_fap_vm_free(vm, bit_buffer->guest_buf)) return false;
        memset(bit_buffer, 0, sizeof(*bit_buffer)); break;
    case ArmImport_bit_buffer_reset: bit_buffer_reset(bit_buffer->native); break;
    case ArmImport_bit_buffer_append_byte:
        if(bit_buffer_get_size_bytes(bit_buffer->native) >= bit_buffer->capacity) {
            fault(a, "ARM BitBuffer capacity exceeded"); return false;
        }
        bit_buffer_append_byte(bit_buffer->native, q); break;
    case ArmImport_bit_buffer_append_bytes: {
        if(r > bit_buffer->capacity - bit_buffer_get_size_bytes(bit_buffer->native)) {
            fault(a, "ARM BitBuffer capacity exceeded"); return false;
        }
        if(!r) break;
        const uint8_t* bytes = arm_fap_vm_pointer(vm, q, r, false); if(!bytes) return false;
        bit_buffer_append_bytes(bit_buffer->native, bytes, r); break;
    }
    case ArmImport_bit_buffer_get_data: {
        uint32_t size = bit_buffer_get_size_bytes(bit_buffer->native);
        void* dst = arm_fap_vm_pointer(vm, bit_buffer->guest_buf, bit_buffer->capacity, true);
        if(!dst) return false;
        memcpy(dst, bit_buffer_get_data(bit_buffer->native), size);
        result = bit_buffer->guest_buf; break;
    }
    case ArmImport_bit_buffer_get_size_bytes: result = bit_buffer_get_size_bytes(bit_buffer->native); break;
    case ArmImport_file_stream_alloc:
    case ArmImport_buffered_file_stream_alloc: {
        if(!has_storage(a, p)) return false;
        Stream* native = id == ArmImport_file_stream_alloc ? file_stream_alloc(a->storage) : buffered_file_stream_alloc(a->storage);
        result = wrap_stream(a, native, true);
        if(!result) { if(native) stream_free(native); break; }
        get_stream(a, result)->kind = id == ArmImport_file_stream_alloc ? 1 : 2; break;
    }
    case ArmImport_file_stream_open:
    case ArmImport_buffered_file_stream_open:
    case ArmImport_file_stream_close:
    case ArmImport_buffered_file_stream_close: {
        ArmStream* owned = get_stream(a, p); if(!owned) return false;
        bool buffered = id == ArmImport_buffered_file_stream_open || id == ArmImport_buffered_file_stream_close;
        if(owned->kind != (buffered ? 2 : 1) || !owned->owned) return false;
        if(id == ArmImport_file_stream_open || id == ArmImport_buffered_file_stream_open) {
            const char* path = arm_fap_vm_string(vm, q);
            if(!path || r < FSAM_READ || r > FSAM_READ_WRITE ||
               (s != FSOM_OPEN_EXISTING && s != FSOM_OPEN_ALWAYS && s != FSOM_OPEN_APPEND &&
                s != FSOM_CREATE_NEW && s != FSOM_CREATE_ALWAYS)) return false;
            GUI_CALL(result = buffered ? buffered_file_stream_open(owned->native, path, r, s) : file_stream_open(owned->native, path, r, s));
        } else { GUI_CALL(result = buffered ? buffered_file_stream_close(owned->native) : file_stream_close(owned->native)); }
        break;
    }
    case ArmImport_dir_walk_alloc:
        if(!has_storage(a, p)) return false;
        for(unsigned i = 0; i < MAX_DIR_WALKS; i++) if(!a->dir_walks[i].native) {
            GUI_CALL(a->dir_walks[i].native = dir_walk_alloc(a->storage));
            if(a->dir_walks[i].native) result = DIR_WALK_HANDLE + i * 4;
            break;
        }
        break;
    case ArmImport_dir_walk_free:
        GUI_CALL(dir_walk_free(dir_walk->native)); memset(dir_walk, 0, sizeof(*dir_walk)); break;
    case ArmImport_dir_walk_open: {
        const char* path = arm_fap_vm_string(vm, q); if(!path) return false;
        GUI_CALL(result = dir_walk_open(dir_walk->native, path));
        dir_walk->opened = result; break;
    }
    case ArmImport_dir_walk_set_recursive: dir_walk_set_recursive(dir_walk->native, q != 0); break;
    case ArmImport_dir_walk_read: {
        ArmString* path = get_string(a, q); if(!path || !dir_walk->opened) return false;
        /* ARM aligns the uint64 size to offset 8, after flags and padding. */
        if(r && !arm_fap_vm_pointer(vm, r, 16, true)) return false;
        FileInfo info = {0};
        GUI_CALL(result = dir_walk_read(dir_walk->native, path->native, r ? &info : NULL));
        if(r && result == DirWalkOK) {
            arm_fap_vm_write(vm, r, info.flags, 1);
            arm_fap_vm_write(vm, r + 8, info.size, 4);
            arm_fap_vm_write(vm, r + 12, (uint64_t)info.size >> 32, 4);
        }
        break;
    }
    case ArmImport_memmgr_get_free_heap: result = memmgr_get_free_heap(); break;
    case ArmImport_dolphin_deed:
        if(p >= DolphinDeedMAX) return false;
        GUI_CALL(dolphin_deed(p)); break;
    case ArmImport_args_read_int_and_trim: {
        ArmString* args = get_string(a, p);
        if(!args || !arm_fap_vm_pointer(vm, q, 4, true)) return false;
        int value; result = args_read_int_and_trim(args->native, &value);
        if(result && !arm_fap_vm_write(vm, q, value, 4)) return false;
        break;
    }
    case ArmImport_args_read_string_and_trim:
    case ArmImport_args_read_probably_quoted_string_and_trim: {
        ArmString* args = get_string(a, p); ArmString* word = get_string(a, q);
        if(!args || !word || args == word) return false;
        const char* text = furi_string_get_cstr(args->native);
        /* Native parser assumes that a leading quote has a closing quote. */
        if(id == ArmImport_args_read_probably_quoted_string_and_trim && text[0] == '"' && !strchr(text+1, '"')) break;
        result = id == ArmImport_args_read_string_and_trim ? args_read_string_and_trim(args->native, word->native) :
            args_read_probably_quoted_string_and_trim(args->native, word->native); break;
    }
    case ArmImport_path_extract_filename_no_ext: {
        const char* path = arm_fap_vm_string(vm, p); ArmString* name = get_string(a, q);
        if(!path || !name) return false;
        path_extract_filename_no_ext(path, name->native); break;
    }
    case ArmImport_path_extract_filename: {
        ArmString* path = get_string(a, p); ArmString* name = get_string(a, q);
        if(!path || !name || path == name) return false;
        path_extract_filename(path->native, name->native, r != 0); break;
    }
    case ArmImport_path_extract_extension: {
        ArmString* path = get_string(a, p);
        if(!path || !r) return false;
        char* ext = arm_fap_vm_pointer(vm, q, r, true); if(!ext) return false;
        path_extract_extension(path->native, ext, r); break;
    }
    case ArmImport_saved_struct_load:
    case ArmImport_saved_struct_save: {
        const char* path = arm_fap_vm_string(vm, p);
        uint32_t version = arm_fap_vm_read(vm, vm->r[13], 4);
        if(!path || !r || vm->error[0]) return false;
        void* bytes = arm_fap_vm_pointer(vm, q, r, id == ArmImport_saved_struct_load);
        if(!bytes) return false;
        if(id == ArmImport_saved_struct_load) { GUI_CALL(result = saved_struct_load(path, bytes, r, s, version)); }
        else { GUI_CALL(result = saved_struct_save(path, bytes, r, s, version)); }
        break;
    }
    case ArmImport_pretty_format_bytes_hex_canonical: {
        ArmString* output = get_string(a, p);
        uint32_t size = arm_fap_vm_read(vm, vm->r[13], 4);
        const char* prefix = r ? arm_fap_vm_string(vm, r) : NULL;
        if(!output || !q || q > 256 || vm->error[0] || (r && !prefix)) return false;
        uint8_t empty = 0;
        const uint8_t* bytes = size ? arm_fap_vm_pointer(vm, s, size, false) : &empty;
        if(!bytes) return false;
        pretty_format_bytes_hex_canonical(output->native, q, prefix, bytes, size); break;
    }
    case ArmImport_datetime_get_days_per_month:
        if(q < 1 || q > 12) return false;
        result = datetime_get_days_per_month(p != 0, q); break;
    case ArmImport_datetime_get_days_per_year: result = datetime_get_days_per_year(p); break;
    case ArmImport_datetime_is_leap_year: result = datetime_is_leap_year(p); break;
    case ArmImport_datetime_datetime_to_timestamp: {
        DateTime date; if(!guest_datetime(vm, p, &date)) return false;
        result = datetime_datetime_to_timestamp(&date); break;
    }
    case ArmImport_datetime_timestamp_to_datetime: {
        if(!arm_fap_vm_pointer(vm, q, 10, true)) return false;
        DateTime date; datetime_timestamp_to_datetime(p, &date);
        arm_fap_vm_write(vm, q, date.hour, 1); arm_fap_vm_write(vm, q+1, date.minute, 1);
        arm_fap_vm_write(vm, q+2, date.second, 1); arm_fap_vm_write(vm, q+3, date.day, 1);
        arm_fap_vm_write(vm, q+4, date.month, 1); arm_fap_vm_write(vm, q+6, date.year, 2);
        arm_fap_vm_write(vm, q+8, date.weekday, 1); break;
    }
    case ArmImport_locale_get_date_format: result = locale_get_date_format(); break;
    case ArmImport_locale_get_time_format: result = locale_get_time_format(); break;
    case ArmImport_locale_celsius_to_fahrenheit:
    case ArmImport_locale_fahrenheit_to_celsius: {
        float value; memcpy(&value, &p, 4);
        value = id == ArmImport_locale_celsius_to_fahrenheit ? locale_celsius_to_fahrenheit(value) : locale_fahrenheit_to_celsius(value);
        memcpy(&result, &value, 4); break;
    }
    case ArmImport_locale_format_date:
    case ArmImport_locale_format_time: {
        ArmString* output = get_string(a, p); DateTime date;
        if(!output || !guest_datetime(vm, q, &date)) return false;
        if(id == ArmImport_locale_format_date) {
            const char* separator = arm_fap_vm_string(vm, s);
            if(!separator || r > LocaleDateFormatYMD) return false;
            locale_format_date(output->native, &date, r, separator);
        } else {
            if(r > LocaleTimeFormat12h) return false;
            locale_format_time(output->native, &date, r, s != 0);
        }
        break;
    }
    case ArmImport_manchester_advance: {
        if(p > ManchesterStateStart0 || q > ManchesterEventReset || (q & 1) ||
           !arm_fap_vm_pointer(vm, r, 1, true) || (s && !arm_fap_vm_pointer(vm, s, 1, true))) return false;
        ManchesterState next; bool data = false;
        result = manchester_advance(p, q, &next, s ? &data : NULL);
        arm_fap_vm_write(vm, r, next, 1); /* ARM short enum, not native enum width. */
        if(s && result) arm_fap_vm_write(vm, s, data, 1);
        break;
    }
    case ArmImport_malloc: result = arm_fap_vm_alloc(vm, p); break;
    case ArmImport_free: return arm_fap_vm_free(vm, p);
    case ArmImport___furi_crash_implementation: fault(a, "ARM app called furi_check/crash"); return false;
    case ArmImport_furi_hal_random_get: result = furi_hal_random_get(); break;
    case ArmImport_furi_record_open: {
        const char* name = arm_fap_vm_string(vm, p);
        if(!name) return false;
        if(!strcmp(name, RECORD_GUI)) {
            a->gui = furi_record_open(RECORD_GUI); a->gui_refs++; result = GUI_HANDLE;
        } else if(!strcmp(name, RECORD_NOTIFICATION)) {
            a->notification = furi_record_open(RECORD_NOTIFICATION);
            a->notification_refs++; result = NOTIFICATION_HANDLE;
        } else if(!strcmp(name, RECORD_DIALOGS)) {
            a->dialogs = furi_record_open(RECORD_DIALOGS);
            a->dialogs_refs++; result = DIALOGS_HANDLE;
        } else if(!strcmp(name, RECORD_STORAGE)) {
            a->storage = furi_record_open(RECORD_STORAGE);
            a->storage_refs++; result = STORAGE_HANDLE;
        } else return false;
        break;
    }
    case ArmImport_furi_record_close: {
        const char* name = arm_fap_vm_string(vm, p);
        if(!name) return false;
        if(!strcmp(name, RECORD_GUI) && a->gui_refs) {
            furi_record_close(RECORD_GUI); a->gui_refs--;
        } else if(!strcmp(name, RECORD_NOTIFICATION) && a->notification_refs) {
            furi_record_close(RECORD_NOTIFICATION); a->notification_refs--;
        } else if(!strcmp(name, RECORD_DIALOGS) && a->dialogs_refs) {
            furi_record_close(RECORD_DIALOGS); a->dialogs_refs--;
        } else if(!strcmp(name, RECORD_STORAGE) && a->storage_refs) {
            furi_record_close(RECORD_STORAGE); a->storage_refs--;
        } else return false;
        break;
    }
    case ArmImport_view_alloc:
        for(unsigned i = 0; i < MAX_VIEWS; i++) if(!a->views[i].native) {
            v = &a->views[i]; v->owner = a; v->native = view_alloc();
            view_set_context(v->native, v);
            result = VIEW_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_view_free:
        if(v->added || v->external) return false;
        view_free(v->native);
        if(v->model) arm_fap_vm_free(vm, v->model);
        memset(v, 0, sizeof(*v)); break;
    case ArmImport_view_allocate_model:
        if(v->model || (q != 1 && q != 2) || !r) return false;
        v->model = arm_fap_vm_alloc(vm, r); if(!v->model) return false;
        /* Guest model transactions are serialized by the interpreter mutex.
         * Native draw must not acquire a model mutex before that mutex. */
        view_allocate_model(v->native, ViewModelTypeLockFree, sizeof(ArmView*));
        *(ArmView**)view_get_model(v->native) = v;
        break;
    case ArmImport_view_get_model:
        if(!v->model) return false;
        result = v->model; break;
    case ArmImport_view_commit_model:
        GUI_CALL(view_commit_model(v->native, q != 0)); break;
    case ArmImport_view_set_context: v->context = q; break;
    case ArmImport_view_set_draw_callback:
        if(!v->model) return false;
        v->draw = q; view_set_draw_callback(v->native, q ? draw_callback : NULL); break;
    case ArmImport_view_set_input_callback:
        v->input = q; view_set_input_callback(v->native, q ? input_callback : NULL); break;
    case ArmImport_view_free_model:
        if(!v->model) return false;
        view_free_model(v->native);
        arm_fap_vm_free(vm, v->model); v->model = 0; break;
    case ArmImport_view_set_enter_callback:
        v->enter = q; view_set_enter_callback(v->native, q ? enter_callback : NULL); break;
    case ArmImport_view_set_exit_callback:
        v->exit = q; view_set_exit_callback(v->native, q ? exit_callback : NULL); break;
    case ArmImport_view_set_previous_callback:
        v->previous = q; view_set_previous_callback(v->native, q ? previous_callback : NULL); break;
    case ArmImport_view_set_orientation:
        if(q > ViewOrientationVerticalFlip) return false;
        view_set_orientation(v->native, (ViewOrientation)q); break;
    case ArmImport_view_dispatcher_alloc:
        if(a->dispatcher) return false;
        a->dispatcher = view_dispatcher_alloc();
        view_dispatcher_set_event_callback_context(a->dispatcher, a);
        result = DISPATCHER_HANDLE; break;
    case ArmImport_view_dispatcher_free:
        if(a->running) return false;
        for(unsigned i = 0; i < MAX_VIEWS; i++) if(a->views[i].added) return false;
        GUI_CALL(view_dispatcher_free(a->dispatcher)); a->dispatcher = NULL; break;
    case ArmImport_view_dispatcher_set_event_callback_context: a->event_context = q; break;
    case ArmImport_view_dispatcher_set_navigation_event_callback:
        a->navigate = q;
        view_dispatcher_set_navigation_event_callback(a->dispatcher, q ? navigation_callback : NULL); break;
    case ArmImport_view_dispatcher_set_tick_event_callback:
        if(q && (!r || r > 60000)) return false;
        a->tick = q;
        view_dispatcher_set_tick_event_callback(a->dispatcher, q ? tick_callback : NULL, r); break;
    case ArmImport_view_dispatcher_add_view:
        v = get_view(a, r); if(!v || v->added || q >= 0xfffffffeu) return false;
        for(unsigned i = 0; i < MAX_VIEWS; i++) if(a->views[i].added && a->views[i].id == q) return false;
        GUI_CALL(view_dispatcher_add_view(a->dispatcher, q, v->native)); v->id = q; v->added = true; break;
    case ArmImport_view_dispatcher_remove_view:
        for(unsigned i = 0; i < MAX_VIEWS; i++) if(a->views[i].added && a->views[i].id == q) v = &a->views[i];
        if(!v) return false;
        GUI_CALL(view_dispatcher_remove_view(a->dispatcher, q)); v->added = false; break;
    case ArmImport_view_dispatcher_attach_to_gui:
        if(q != GUI_HANDLE || !a->gui_refs || r > 2) return false;
        GUI_CALL(view_dispatcher_attach_to_gui(a->dispatcher, a->gui, (ViewDispatcherType)r)); break;
    case ArmImport_view_dispatcher_switch_to_view:
        for(unsigned i = 0; i < MAX_VIEWS; i++) if(a->views[i].added && a->views[i].id == q) v = &a->views[i];
        if(!v) return false;
        GUI_CALL(view_dispatcher_switch_to_view(a->dispatcher, q)); break;
    case ArmImport_view_dispatcher_run:
        if(a->running || a->canvas) return false;
        a->running = true;
        GUI_CALL(view_dispatcher_run(a->dispatcher)); a->running = false; break;
    case ArmImport_view_dispatcher_stop: view_dispatcher_stop(a->dispatcher); break;
    case ArmImport_view_dispatcher_enable_queue: view_dispatcher_enable_queue(a->dispatcher); break;
    case ArmImport_view_dispatcher_send_custom_event:
        GUI_CALL(view_dispatcher_send_custom_event(a->dispatcher, q)); break;
    case ArmImport_view_dispatcher_set_custom_event_callback:
        a->custom_event = q;
        view_dispatcher_set_custom_event_callback(a->dispatcher, q ? custom_event_callback : NULL);
        break;
    case ArmImport_view_holder_alloc:
        if(a->holder.native) return false;
        a->holder.native = view_holder_alloc();
        result = HOLDER_HANDLE; break;
    case ArmImport_view_holder_free:
        GUI_CALL(view_holder_free(a->holder.native)); memset(&a->holder, 0, sizeof(a->holder)); break;
    case ArmImport_view_holder_set_view:
        v = get_view(a, q); if(!v) return false;
        view_holder_set_view(a->holder.native, v->native); break;
    case ArmImport_view_holder_set_back_callback:
        a->holder.back = q; a->holder.back_context = r;
        view_holder_set_back_callback(a->holder.native, q ? holder_back_callback : NULL, a); break;
    case ArmImport_view_holder_attach_to_gui:
        if(q != GUI_HANDLE || !a->gui_refs) return false;
        GUI_CALL(view_holder_attach_to_gui(a->holder.native, a->gui)); break;
    case ArmImport_submenu_alloc:
        for(unsigned i = 0; i < MAX_SUBMENUS; i++) if(!a->submenus[i].native) {
            a->submenus[i].owner = a; a->submenus[i].native = submenu_alloc();
            result = SUBMENU_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_submenu_free: {
        ArmView* view = sub->view_handle ? get_view(a, sub->view_handle) : NULL;
        if(view && view->added) return false;
        GUI_CALL(submenu_free(sub->native));
        if(view) memset(view, 0, sizeof(*view));
        memset(sub, 0, sizeof(*sub));
        break;
    }
    case ArmImport_submenu_get_view:
        if(!sub->view_handle) {
            sub->view_handle = wrap_view(a, submenu_get_view(sub->native));
            if(!sub->view_handle) return false;
        }
        result = sub->view_handle; break;
    case ArmImport_submenu_add_item: {
        const char* label = arm_fap_vm_string(vm, q);
        uint32_t context = arm_fap_vm_read(vm, vm->r[13], 4);
        if(!label || !s || vm->error[0]) return false;
        sub->callback = s; sub->context = context;
        submenu_add_item(sub->native, label, r, submenu_item_trampoline, sub);
        break;
    }
    case ArmImport_submenu_change_item_label: {
        const char* label = arm_fap_vm_string(vm, r);
        if(!label) return false;
        submenu_change_item_label(sub->native, q, label); break;
    }
    case ArmImport_submenu_reset: submenu_reset(sub->native); break;
    case ArmImport_submenu_set_header: {
        const char* header = arm_fap_vm_string(vm, q);
        if(!header) return false;
        submenu_set_header(sub->native, header); break;
    }
    case ArmImport_submenu_set_selected_item: submenu_set_selected_item(sub->native, q); break;
    case ArmImport_text_box_alloc:
        for(unsigned i = 0; i < MAX_TEXTBOXES; i++) if(!a->text_boxes[i].native) {
            a->text_boxes[i].native = text_box_alloc();
            result = TEXTBOX_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_text_box_free: {
        ArmView* view = tbox->view_handle ? get_view(a, tbox->view_handle) : NULL;
        if(view && view->added) return false;
        GUI_CALL(text_box_free(tbox->native));
        if(view) memset(view, 0, sizeof(*view));
        memset(tbox, 0, sizeof(*tbox));
        break;
    }
    case ArmImport_text_box_get_view:
        if(!tbox->view_handle) {
            tbox->view_handle = wrap_view(a, text_box_get_view(tbox->native));
            if(!tbox->view_handle) return false;
        }
        result = tbox->view_handle; break;
    case ArmImport_text_box_reset: text_box_reset(tbox->native); break;
    case ArmImport_text_box_set_text: {
        const char* text = arm_fap_vm_string(vm, q);
        if(!text) return false;
        text_box_set_text(tbox->native, text); break;
    }
    case ArmImport_text_box_set_font:
        if(q > TextBoxFontHex) return false;
        text_box_set_font(tbox->native, (TextBoxFont)q); break;
    case ArmImport_text_box_set_focus:
        if(q > TextBoxFocusEnd) return false;
        text_box_set_focus(tbox->native, (TextBoxFocus)q); break;
    case ArmImport_variable_item_list_alloc:
        for(unsigned i = 0; i < MAX_VARLISTS; i++) if(!a->var_lists[i].native) {
            a->var_lists[i].owner = a; a->var_lists[i].native = variable_item_list_alloc();
            result = VARLIST_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_variable_item_list_free: {
        ArmView* view = list->view_handle ? get_view(a, list->view_handle) : NULL;
        if(view && view->added) return false;
        GUI_CALL(variable_item_list_free(list->native));
        if(view) memset(view, 0, sizeof(*view));
        memset(list, 0, sizeof(*list));
        break;
    }
    case ArmImport_variable_item_list_get_view:
        if(!list->view_handle) {
            list->view_handle = wrap_view(a, variable_item_list_get_view(list->native));
            if(!list->view_handle) return false;
        }
        result = list->view_handle; break;
    case ArmImport_variable_item_list_reset: variable_item_list_reset(list->native); break;
    case ArmImport_variable_item_list_set_selected_item:
        variable_item_list_set_selected_item(list->native, q); break;
    case ArmImport_variable_item_list_set_enter_callback:
        list->enter_callback = q; list->enter_context = r;
        variable_item_list_set_enter_callback(
            list->native, q ? var_list_enter_trampoline : NULL, list);
        break;
    case ArmImport_variable_item_list_add: {
        const char* label = arm_fap_vm_string(vm, q);
        uint32_t context = arm_fap_vm_read(vm, vm->r[13], 4);
        if(!label || !s || vm->error[0]) return false;
        unsigned slot = MAX_ITEMS;
        for(unsigned i = 0; i < MAX_ITEMS; i++) if(!a->items[i].native) { slot = i; break; }
        if(slot == MAX_ITEMS) return false;
        ArmVariableItem* it = &a->items[slot];
        it->owner = a; it->callback = s; it->context = context;
        it->handle = ITEM_HANDLE + slot * 4;
        it->native = variable_item_list_add(
            list->native, label, r, variable_item_change_trampoline, it);
        result = it->handle;
        break;
    }
    case ArmImport_variable_item_get_context: result = item->context; break;
    case ArmImport_variable_item_get_current_value_index:
        result = variable_item_get_current_value_index(item->native); break;
    case ArmImport_variable_item_set_current_value_index:
        variable_item_set_current_value_index(item->native, q); break;
    case ArmImport_variable_item_set_current_value_text: {
        const char* text = arm_fap_vm_string(vm, q);
        if(!text) return false;
        variable_item_set_current_value_text(item->native, text); break;
    }
    case ArmImport_widget_alloc:
        for(unsigned i = 0; i < MAX_WIDGETS; i++) if(!a->widgets[i].native) {
            a->widgets[i].owner = a; a->widgets[i].native = widget_alloc();
            result = WIDGET_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_widget_free: {
        ArmView* view = widget->view_handle ? get_view(a, widget->view_handle) : NULL;
        if(view && view->added) return false;
        GUI_CALL(widget_free(widget->native));
        if(view) memset(view, 0, sizeof(*view));
        memset(widget, 0, sizeof(*widget));
        break;
    }
    case ArmImport_widget_get_view:
        if(!widget->view_handle) {
            widget->view_handle = wrap_view(a, widget_get_view(widget->native));
            if(!widget->view_handle) return false;
        }
        result = widget->view_handle; break;
    case ArmImport_widget_reset: GUI_CALL(widget_reset(widget->native)); break;
    case ArmImport_widget_add_string_element: {
        uint32_t vertical = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t font = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        uint32_t text = arm_fap_vm_read(vm, vm->r[13] + 8, 4);
        const char* string = arm_fap_vm_string(vm, text);
        if(!string || s > AlignCenter || vertical > AlignCenter || font >= FontTotalNumber ||
           vm->error[0]) return false;
        widget_add_string_element(
            widget->native, (uint8_t)q, (uint8_t)r, (Align)s, (Align)vertical, (Font)font, string);
        break;
    }
    case ArmImport_widget_add_string_multiline_element: {
        uint32_t vertical = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t font = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        uint32_t text = arm_fap_vm_read(vm, vm->r[13] + 8, 4);
        const char* string = arm_fap_vm_string(vm, text);
        if(!string || s > AlignCenter || vertical > AlignCenter || font >= FontTotalNumber ||
           vm->error[0]) return false;
        widget_add_string_multiline_element(
            widget->native, (uint8_t)q, (uint8_t)r, (Align)s, (Align)vertical, (Font)font, string);
        break;
    }
    case ArmImport_widget_add_text_box_element: {
        uint32_t height = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t horizontal = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        uint32_t vertical = arm_fap_vm_read(vm, vm->r[13] + 8, 4);
        uint32_t text = arm_fap_vm_read(vm, vm->r[13] + 12, 4);
        uint32_t strip = arm_fap_vm_read(vm, vm->r[13] + 16, 4);
        const char* string = arm_fap_vm_string(vm, text);
        if(!string || horizontal > AlignCenter || vertical > AlignCenter || vm->error[0]) return false;
        widget_add_text_box_element(
            widget->native, (uint8_t)q, (uint8_t)r, (uint8_t)s, (uint8_t)height,
            (Align)horizontal, (Align)vertical, string, strip != 0);
        break;
    }
    case ArmImport_widget_add_text_scroll_element: {
        uint32_t height = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t text = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        const char* string = arm_fap_vm_string(vm, text);
        if(!string || vm->error[0]) return false;
        widget_add_text_scroll_element(widget->native, (uint8_t)q, (uint8_t)r, (uint8_t)s, (uint8_t)height, string);
        break;
    }
    case ArmImport_widget_add_button_element: {
        uint32_t context = arm_fap_vm_read(vm, vm->r[13], 4);
        const char* text = arm_fap_vm_string(vm, r);
        if(!text || q > GuiButtonTypeRight || !s || vm->error[0]) return false;
        widget->callback = s; widget->context = context;
        widget_add_button_element(widget->native, (GuiButtonType)q, text, widget_button_trampoline, widget);
        break;
    }
    case ArmImport_widget_add_rect_element: {
        uint32_t height = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t radius = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        uint32_t fill = arm_fap_vm_read(vm, vm->r[13] + 8, 4);
        if(vm->error[0]) return false;
        widget_add_rect_element(
            widget->native, (uint8_t)q, (uint8_t)r, (uint8_t)s, (uint8_t)height, (uint8_t)radius,
            fill != 0);
        break;
    }
    case ArmImport_text_input_alloc:
        for(unsigned i = 0; i < MAX_TEXT_INPUTS; i++) if(!a->text_inputs[i].native) {
            a->text_inputs[i].owner = a; a->text_inputs[i].native = text_input_alloc();
            result = TEXTINPUT_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_text_input_free: {
        ArmView* view = tinput->view_handle ? get_view(a, tinput->view_handle) : NULL;
        if(view && view->added) return false;
        GUI_CALL(text_input_free(tinput->native));
        if(view) memset(view, 0, sizeof(*view));
        memset(tinput, 0, sizeof(*tinput));
        break;
    }
    case ArmImport_text_input_get_view:
        if(!tinput->view_handle) {
            tinput->view_handle = wrap_view(a, text_input_get_view(tinput->native));
            if(!tinput->view_handle) return false;
        }
        result = tinput->view_handle; break;
    case ArmImport_text_input_reset: text_input_reset(tinput->native); break;
    case ArmImport_text_input_set_result_callback: {
        uint32_t buffer_size = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t clear_default = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        char* buffer = arm_fap_vm_pointer(vm, s, buffer_size, true);
        if(!buffer || !buffer_size || vm->error[0]) return false;
        tinput->result_callback = q; tinput->result_context = r;
        text_input_set_result_callback(
            tinput->native, q ? text_input_result_trampoline : NULL, tinput, buffer, buffer_size,
            clear_default != 0);
        break;
    }
    case ArmImport_text_input_set_minimum_length:
        text_input_set_minimum_length(tinput->native, q); break;
    case ArmImport_text_input_set_header_text: {
        const char* text = arm_fap_vm_string(vm, q);
        if(!text) return false;
        text_input_set_header_text(tinput->native, text); break;
    }
    case ArmImport_dialog_message_alloc:
        for(unsigned i = 0; i < MAX_DIALOG_MESSAGES; i++) if(!a->dialog_messages[i].native) {
            a->dialog_messages[i].native = dialog_message_alloc();
            result = DIALOG_MESSAGE_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_dialog_message_free:
        dialog_message_free(dmsg->native); memset(dmsg, 0, sizeof(*dmsg)); break;
    case ArmImport_dialog_message_set_header:
    case ArmImport_dialog_message_set_text: {
        uint32_t horizontal = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t vertical = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        const char* text = q ? arm_fap_vm_string(vm, q) : NULL;
        if((q && !text) || horizontal > AlignCenter || vertical > AlignCenter || vm->error[0])
            return false;
        if(id == ArmImport_dialog_message_set_header)
            dialog_message_set_header(
                dmsg->native, text, (uint8_t)r, (uint8_t)s, (Align)horizontal, (Align)vertical);
        else
            dialog_message_set_text(
                dmsg->native, text, (uint8_t)r, (uint8_t)s, (Align)horizontal, (Align)vertical);
        break;
    }
    case ArmImport_dialog_message_set_buttons: {
        const char* left = q ? arm_fap_vm_string(vm, q) : NULL;
        const char* center = r ? arm_fap_vm_string(vm, r) : NULL;
        const char* right = s ? arm_fap_vm_string(vm, s) : NULL;
        if((q && !left) || (r && !center) || (s && !right)) return false;
        dialog_message_set_buttons(dmsg->native, left, center, right); break;
    }
    case ArmImport_dialog_message_show: {
        if(p != DIALOGS_HANDLE || !a->dialogs_refs) return false;
        ArmDialogMessage* message = get_dialog_message(a, q);
        if(!message) return false;
        DialogMessageButton button;
        GUI_CALL(button = dialog_message_show(a->dialogs, message->native));
        result = (uint32_t)button; break;
    }
    case ArmImport_dialog_message_show_storage_error: {
        if(p != DIALOGS_HANDLE || !a->dialogs_refs) return false;
        const char* text = arm_fap_vm_string(vm, q);
        if(!text) return false;
        GUI_CALL(dialog_message_show_storage_error(a->dialogs, text)); break;
    }
    case ArmImport_dialog_ex_alloc:
        for(unsigned i = 0; i < MAX_DIALOG_EX; i++) if(!a->dialog_exs[i].native) {
            a->dialog_exs[i].owner = a; a->dialog_exs[i].native = dialog_ex_alloc();
            dialog_ex_set_context(a->dialog_exs[i].native, &a->dialog_exs[i]);
            result = DIALOGEX_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_dialog_ex_free: {
        ArmView* view = dex->view_handle ? get_view(a, dex->view_handle) : NULL;
        if(view && view->added) return false;
        GUI_CALL(dialog_ex_free(dex->native));
        if(view) memset(view, 0, sizeof(*view));
        memset(dex, 0, sizeof(*dex));
        break;
    }
    case ArmImport_dialog_ex_get_view:
        if(!dex->view_handle) {
            dex->view_handle = wrap_view(a, dialog_ex_get_view(dex->native));
            if(!dex->view_handle) return false;
        }
        result = dex->view_handle; break;
    case ArmImport_dialog_ex_reset: dialog_ex_reset(dex->native); break;
    case ArmImport_dialog_ex_set_context: dex->context = q; break;
    case ArmImport_dialog_ex_set_result_callback:
        dex->callback = q;
        dialog_ex_set_result_callback(dex->native, q ? dialog_ex_result_trampoline : NULL);
        break;
    case ArmImport_dialog_ex_set_header:
    case ArmImport_dialog_ex_set_text: {
        uint32_t horizontal = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t vertical = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        const char* text = q ? arm_fap_vm_string(vm, q) : NULL;
        if((q && !text) || horizontal > AlignCenter || vertical > AlignCenter || vm->error[0])
            return false;
        if(id == ArmImport_dialog_ex_set_header)
            dialog_ex_set_header(
                dex->native, text, (uint8_t)r, (uint8_t)s, (Align)horizontal, (Align)vertical);
        else
            dialog_ex_set_text(
                dex->native, text, (uint8_t)r, (uint8_t)s, (Align)horizontal, (Align)vertical);
        break;
    }
    case ArmImport_dialog_ex_set_left_button_text:
    case ArmImport_dialog_ex_set_center_button_text:
    case ArmImport_dialog_ex_set_right_button_text: {
        const char* text = q ? arm_fap_vm_string(vm, q) : NULL;
        if(q && !text) return false;
        if(id == ArmImport_dialog_ex_set_left_button_text)
            dialog_ex_set_left_button_text(dex->native, text);
        else if(id == ArmImport_dialog_ex_set_center_button_text)
            dialog_ex_set_center_button_text(dex->native, text);
        else
            dialog_ex_set_right_button_text(dex->native, text);
        break;
    }
    case ArmImport_scene_manager_alloc: {
        if(a->scene_managers[0].native) return false;
        uint32_t enter_addr = arm_fap_vm_read(vm, p, 4);
        uint32_t event_addr = arm_fap_vm_read(vm, p + 4, 4);
        uint32_t exit_addr = arm_fap_vm_read(vm, p + 8, 4);
        uint32_t scene_num = arm_fap_vm_read(vm, p + 12, 4);
        if(!scene_num || scene_num > MAX_SCENES || vm->error[0]) return false;
        ArmSceneManager* sm = &a->scene_managers[0];
        for(uint32_t i = 0; i < scene_num; i++) {
            sm->on_enter[i] = arm_fap_vm_read(vm, enter_addr + i * 4, 4);
            sm->on_event[i] = arm_fap_vm_read(vm, event_addr + i * 4, 4);
            sm->on_exit[i] = arm_fap_vm_read(vm, exit_addr + i * 4, 4);
        }
        if(vm->error[0]) return false;
        sm->owner = a; sm->context = q; sm->scene_num = scene_num;
        const SceneManagerHandlers real_handlers = {
            .on_enter_handlers = SCENE_ENTER_TABLE,
            .on_event_handlers = SCENE_EVENT_TABLE,
            .on_exit_handlers = SCENE_EXIT_TABLE,
            .scene_num = scene_num,
        };
        sm->native = scene_manager_alloc(&real_handlers, sm);
        result = SCENE_MANAGER_HANDLE; break;
    }
    case ArmImport_scene_manager_free:
        scene_manager_free(a->scene_managers[0].native);
        memset(&a->scene_managers[0], 0, sizeof(a->scene_managers[0]));
        break;
    case ArmImport_scene_manager_get_scene_state:
        result = scene_manager_get_scene_state(a->scene_managers[0].native, q); break;
    case ArmImport_scene_manager_handle_back_event:
        result = scene_manager_handle_back_event(a->scene_managers[0].native) ? 1 : 0; break;
    case ArmImport_scene_manager_handle_custom_event:
        result = scene_manager_handle_custom_event(a->scene_managers[0].native, q) ? 1 : 0; break;
    case ArmImport_scene_manager_handle_tick_event:
        scene_manager_handle_tick_event(a->scene_managers[0].native); break;
    case ArmImport_scene_manager_has_previous_scene:
        result = scene_manager_has_previous_scene(a->scene_managers[0].native, q) ? 1 : 0; break;
    case ArmImport_scene_manager_next_scene:
        scene_manager_next_scene(a->scene_managers[0].native, q); break;
    case ArmImport_scene_manager_previous_scene:
        result = scene_manager_previous_scene(a->scene_managers[0].native) ? 1 : 0; break;
    case ArmImport_scene_manager_search_and_switch_to_another_scene:
        result = scene_manager_search_and_switch_to_another_scene(a->scene_managers[0].native, q) ?
                 1 : 0;
        break;
    case ArmImport_scene_manager_search_and_switch_to_previous_scene:
        result = scene_manager_search_and_switch_to_previous_scene(a->scene_managers[0].native, q) ?
                 1 : 0;
        break;
    case ArmImport_scene_manager_search_and_switch_to_previous_scene_one_of: {
        if(r > MAX_SCENES) return false;
        const uint32_t* ids = arm_fap_vm_pointer(vm, q, r * 4, false);
        if(!ids) return false;
        result = scene_manager_search_and_switch_to_previous_scene_one_of(
                     a->scene_managers[0].native, ids, r) ? 1 : 0;
        break;
    }
    case ArmImport_scene_manager_set_scene_state:
        scene_manager_set_scene_state(a->scene_managers[0].native, q, r); break;
    case ArmImport_scene_manager_stop: scene_manager_stop(a->scene_managers[0].native); break;
    case ArmImport_infrared_get_protocol_address_length:
        if((int32_t)p < InfraredProtocolUnknown || (int32_t)p >= InfraredProtocolMAX) return false;
        result = infrared_get_protocol_address_length((InfraredProtocol)p); break;
    case ArmImport_infrared_get_protocol_command_length:
        if((int32_t)p < InfraredProtocolUnknown || (int32_t)p >= InfraredProtocolMAX) return false;
        result = infrared_get_protocol_command_length((InfraredProtocol)p); break;
    case ArmImport_infrared_is_protocol_valid:
        if((int32_t)p < InfraredProtocolUnknown || (int32_t)p >= InfraredProtocolMAX) return false;
        result = infrared_is_protocol_valid((InfraredProtocol)p) ? 1 : 0; break;
    case ArmImport_infrared_get_protocol_by_name: {
        const char* name = arm_fap_vm_string(vm, p);
        if(!name) return false;
        result = (uint32_t)(int32_t)infrared_get_protocol_by_name(name); break;
    }
    case ArmImport_infrared_get_protocol_name: {
        if((int32_t)p < InfraredProtocolUnknown || (int32_t)p >= InfraredProtocolMAX) return false;
        const char* name = infrared_get_protocol_name((InfraredProtocol)p);
        if(!name) return false;
        if(!a->infrared.name_buf) a->infrared.name_buf = arm_fap_vm_alloc(vm, 32);
        if(!a->infrared.name_buf) return false;
        char* dest = arm_fap_vm_pointer(vm, a->infrared.name_buf, 32, true);
        if(!dest) return false;
        size_t n = strlen(name); if(n > 31) n = 31;
        memcpy(dest, name, n); dest[n] = 0;
        result = a->infrared.name_buf; break;
    }
    case ArmImport_infrared_send: {
        int32_t protocol = (int32_t)arm_fap_vm_read(vm, p, 4);
        uint32_t address = arm_fap_vm_read(vm, p + 4, 4);
        uint32_t command = arm_fap_vm_read(vm, p + 8, 4);
        uint32_t repeat = arm_fap_vm_read(vm, p + 12, 1);
        if(protocol < InfraredProtocolUnknown || protocol >= InfraredProtocolMAX || vm->error[0])
            return false;
        InfraredMessage msg = {(InfraredProtocol)protocol, address, command, repeat != 0};
        GUI_CALL(infrared_send(&msg, (int)q));
        break;
    }
    case ArmImport_infrared_send_raw_ext: {
        uint32_t duty_bits = arm_fap_vm_read(vm, vm->r[13], 4);
        float duty_cycle; memcpy(&duty_cycle, &duty_bits, 4);
        if(!q || q > IR_RAW_BUF_COUNT || vm->error[0]) return false;
        const uint32_t* timings = arm_fap_vm_pointer(vm, p, q * 4, false);
        if(!timings) return false;
        GUI_CALL(infrared_send_raw_ext((uint32_t*)timings, q, r != 0, s, duty_cycle));
        break;
    }
    case ArmImport_infrared_worker_alloc:
        if(a->infrared.native) return false;
        a->infrared.owner = a;
        a->infrared.native = infrared_worker_alloc();
        result = INFRARED_HANDLE; break;
    case ArmImport_infrared_worker_free:
        if(!has_infrared(a, p) || a->infrared.rx_running) return false;
        infrared_worker_free(a->infrared.native);
        memset(&a->infrared, 0, sizeof(a->infrared));
        break;
    case ArmImport_infrared_worker_rx_set_received_signal_callback:
        if(!has_infrared(a, p)) return false;
        a->infrared.callback = q; a->infrared.context = r;
        infrared_worker_rx_set_received_signal_callback(
            a->infrared.native, q ? infrared_rx_trampoline : NULL, &a->infrared);
        break;
    case ArmImport_infrared_worker_rx_start:
        if(!has_infrared(a, p) || a->infrared.rx_running) return false;
        infrared_worker_rx_start(a->infrared.native); a->infrared.rx_running = true; break;
    case ArmImport_infrared_worker_rx_stop:
        if(!has_infrared(a, p) || !a->infrared.rx_running) return false;
        infrared_worker_rx_stop(a->infrared.native); a->infrared.rx_running = false; break;
    case ArmImport_infrared_worker_signal_is_decoded:
        if(p != SIGNAL_HANDLE || !a->infrared.current_signal) return false;
        result = infrared_worker_signal_is_decoded(a->infrared.current_signal) ? 1 : 0; break;
    case ArmImport_infrared_worker_get_decoded_signal: {
        if(p != SIGNAL_HANDLE || !a->infrared.current_signal) return false;
        const InfraredMessage* msg = infrared_worker_get_decoded_signal(a->infrared.current_signal);
        if(!msg) return false;
        if(!a->infrared.message_buf)
            a->infrared.message_buf = arm_fap_vm_alloc(vm, IR_MESSAGE_BUF_SIZE);
        if(!a->infrared.message_buf) return false;
        arm_fap_vm_write(vm, a->infrared.message_buf, (uint32_t)(int32_t)msg->protocol, 4);
        arm_fap_vm_write(vm, a->infrared.message_buf + 4, msg->address, 4);
        arm_fap_vm_write(vm, a->infrared.message_buf + 8, msg->command, 4);
        arm_fap_vm_write(vm, a->infrared.message_buf + 12, msg->repeat ? 1 : 0, 1);
        result = a->infrared.message_buf; break;
    }
    case ArmImport_infrared_worker_get_raw_signal: {
        if(p != SIGNAL_HANDLE || !a->infrared.current_signal) return false;
        const uint32_t* native_timings = NULL;
        size_t count = 0;
        infrared_worker_get_raw_signal(a->infrared.current_signal, &native_timings, &count);
        if(count > IR_RAW_BUF_COUNT) count = IR_RAW_BUF_COUNT;
        if(!a->infrared.raw_buf) a->infrared.raw_buf = arm_fap_vm_alloc(vm, IR_RAW_BUF_COUNT * 4);
        if(!a->infrared.raw_buf) return false;
        uint8_t* dest = count ? arm_fap_vm_pointer(vm, a->infrared.raw_buf, count * 4, true) : NULL;
        if(count && !dest) return false;
        if(count) memcpy(dest, native_timings, count * 4);
        if(!arm_fap_vm_write(vm, q, a->infrared.raw_buf, 4)) return false;
        if(!arm_fap_vm_write(vm, r, (uint32_t)count, 4)) return false;
        break;
    }
    case ArmImport_nfc_alloc: {
        Nfc* nfc = nfc_alloc();
        result = wrap_nfc_ptr(a, nfc, NfcPtrNfc, true);
        if(!result) { if(nfc) nfc_free(nfc); return false; }
        break;
    }
    case ArmImport_nfc_free: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrNfc); if(!ptr || !ptr->owned) return false;
        GUI_CALL(nfc_free((Nfc*)ptr->native));
        memset(ptr, 0, sizeof(*ptr)); break;
    }
    case ArmImport_nfc_device_alloc: {
        NfcDevice* dev = nfc_device_alloc();
        result = wrap_nfc_ptr(a, dev, NfcPtrDevice, true);
        if(!result) { if(dev) nfc_device_free(dev); return false; }
        break;
    }
    case ArmImport_nfc_device_free: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrDevice); if(!ptr || !ptr->owned) return false;
        nfc_device_free((NfcDevice*)ptr->native);
        memset(ptr, 0, sizeof(*ptr)); break;
    }
    case ArmImport_nfc_device_get_protocol: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrDevice); if(!ptr) return false;
        result = (uint32_t)nfc_device_get_protocol((NfcDevice*)ptr->native); break;
    }
    case ArmImport_nfc_device_get_data: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrDevice); if(!ptr) return false;
        if(q >= NfcProtocolNum) return false;
        const NfcDeviceData* data = nfc_device_get_data((NfcDevice*)ptr->native, (NfcProtocol)q);
        result = wrap_nfc_ptr(a, data, NfcPtrData, false);
        if(!result) return false;
        break;
    }
    case ArmImport_nfc_device_set_data: {
        ArmNfcPtr* dev = get_nfc_ptr(a, p, NfcPtrDevice); if(!dev) return false;
        if(q >= NfcProtocolNum) return false;
        ArmNfcPtr* data = get_nfc_ptr(a, r, NfcPtrData); if(!data) return false;
        nfc_device_set_data((NfcDevice*)dev->native, (NfcProtocol)q, data->native); break;
    }
    case ArmImport_nfc_device_copy_data: {
        ArmNfcPtr* dev = get_nfc_ptr(a, p, NfcPtrDevice); if(!dev) return false;
        if(q >= NfcProtocolNum) return false;
        ArmNfcPtr* data = get_nfc_ptr(a, r, NfcPtrData); if(!data) return false;
        nfc_device_copy_data(
            (const NfcDevice*)dev->native, (NfcProtocol)q, (NfcDeviceData*)data->native);
        break;
    }
    case ArmImport_nfc_device_get_name: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrDevice); if(!ptr) return false;
        if(q > NfcDeviceNameTypeShort) return false;
        const char* name = nfc_device_get_name((NfcDevice*)ptr->native, (NfcDeviceNameType)q);
        if(!name) return false;
        if(!a->nfc_name_buf) a->nfc_name_buf = arm_fap_vm_alloc(vm, 48);
        if(!a->nfc_name_buf) return false;
        char* dest = arm_fap_vm_pointer(vm, a->nfc_name_buf, 48, true);
        if(!dest) return false;
        size_t n = strlen(name); if(n > 47) n = 47;
        memcpy(dest, name, n); dest[n] = 0;
        result = a->nfc_name_buf; break;
    }
    case ArmImport_nfc_device_set_loading_callback: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrDevice); if(!ptr) return false;
        a->nfc_loading_callback = q; a->nfc_loading_context = r;
        nfc_device_set_loading_callback(
            (NfcDevice*)ptr->native, q ? nfc_loading_trampoline : NULL, a);
        break;
    }
    case ArmImport_nfc_poller_alloc: {
        ArmNfcPtr* nfc = get_nfc_ptr(a, p, NfcPtrNfc); if(!nfc) return false;
        if(q >= NfcProtocolNum) return false;
        NfcPoller* poller = nfc_poller_alloc((Nfc*)nfc->native, (NfcProtocol)q);
        result = wrap_nfc_ptr(a, poller, NfcPtrPoller, true);
        if(!result) { if(poller) nfc_poller_free(poller); return false; }
        break;
    }
    case ArmImport_nfc_poller_free: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrPoller); if(!ptr || !ptr->owned) return false;
        GUI_CALL(nfc_poller_free((NfcPoller*)ptr->native));
        memset(ptr, 0, sizeof(*ptr)); break;
    }
    case ArmImport_nfc_poller_get_data: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrPoller); if(!ptr) return false;
        const NfcDeviceData* data = nfc_poller_get_data((NfcPoller*)ptr->native);
        result = wrap_nfc_ptr(a, data, NfcPtrData, false);
        if(!result) return false;
        break;
    }
    case ArmImport_nfc_poller_stop: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrPoller); if(!ptr) return false;
        GUI_CALL(nfc_poller_stop((NfcPoller*)ptr->native)); break;
    }
    case ArmImport_nfc_scanner_alloc: {
        ArmNfcPtr* nfc = get_nfc_ptr(a, p, NfcPtrNfc); if(!nfc) return false;
        NfcScanner* scanner = nfc_scanner_alloc((Nfc*)nfc->native);
        result = wrap_nfc_ptr(a, scanner, NfcPtrScanner, true);
        if(!result) { if(scanner) nfc_scanner_free(scanner); return false; }
        break;
    }
    case ArmImport_nfc_scanner_free: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrScanner); if(!ptr || !ptr->owned) return false;
        GUI_CALL(nfc_scanner_free((NfcScanner*)ptr->native));
        memset(ptr, 0, sizeof(*ptr)); break;
    }
    case ArmImport_nfc_scanner_start: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrScanner); if(!ptr) return false;
        if(!a->nfc_scan_buf) a->nfc_scan_buf = arm_fap_vm_alloc(vm, 8 * 4);
        if(!a->nfc_scan_buf) return false;
        a->nfc_scanner_callback = q; a->nfc_scanner_context = r;
        GUI_CALL(nfc_scanner_start((NfcScanner*)ptr->native, nfc_scanner_trampoline, a));
        break;
    }
    case ArmImport_nfc_scanner_stop: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrScanner); if(!ptr) return false;
        GUI_CALL(nfc_scanner_stop((NfcScanner*)ptr->native)); break;
    }
    case ArmImport_mf_classic_alloc: {
        MfClassicData* data = mf_classic_alloc();
        result = wrap_nfc_ptr(a, data, NfcPtrData, true);
        if(!result) { if(data) mf_classic_free(data); return false; }
        break;
    }
    case ArmImport_mf_classic_free: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrData); if(!ptr || !ptr->owned) return false;
        mf_classic_free((MfClassicData*)ptr->native);
        memset(ptr, 0, sizeof(*ptr)); break;
    }
    case ArmImport_mf_classic_get_total_sectors_num:
        if(p >= MfClassicTypeNum) return false;
        result = mf_classic_get_total_sectors_num((MfClassicType)p); break;
    case ArmImport_mf_classic_get_first_block_num_of_sector:
        result = mf_classic_get_first_block_num_of_sector((uint8_t)p); break;
    case ArmImport_mf_classic_get_sector_by_block:
        result = mf_classic_get_sector_by_block((uint8_t)p); break;
    case ArmImport_mf_classic_get_sector_trailer_num_by_block:
        result = mf_classic_get_sector_trailer_num_by_block((uint8_t)p); break;
    case ArmImport_mf_classic_is_block_read: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrData); if(!ptr) return false;
        result = mf_classic_is_block_read((const MfClassicData*)ptr->native, (uint8_t)q) ? 1 : 0;
        break;
    }
    case ArmImport_mf_classic_is_card_read: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrData); if(!ptr) return false;
        result = mf_classic_is_card_read((const MfClassicData*)ptr->native) ? 1 : 0; break;
    }
    case ArmImport_mf_classic_get_uid: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrData); if(!ptr) return false;
        size_t uid_len = 0;
        const uint8_t* uid = mf_classic_get_uid((const MfClassicData*)ptr->native, &uid_len);
        if(!uid) return false;
        if(uid_len > 10) uid_len = 10;
        if(!a->nfc_uid_buf) a->nfc_uid_buf = arm_fap_vm_alloc(vm, 10);
        if(!a->nfc_uid_buf) return false;
        uint8_t* dest = arm_fap_vm_pointer(vm, a->nfc_uid_buf, 10, true);
        if(!dest) return false;
        memcpy(dest, uid, uid_len);
        if(!arm_fap_vm_write(vm, q, (uint32_t)uid_len, 4)) return false;
        result = a->nfc_uid_buf; break;
    }
    case ArmImport_mf_classic_get_sector_trailer_by_sector: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrData); if(!ptr) return false;
        const MfClassicSectorTrailer* trailer =
            mf_classic_get_sector_trailer_by_sector((const MfClassicData*)ptr->native, (uint8_t)q);
        if(!trailer) return false;
        if(!a->nfc_trailer_buf) a->nfc_trailer_buf = arm_fap_vm_alloc(vm, sizeof(*trailer));
        if(!a->nfc_trailer_buf) return false;
        uint8_t* dest = arm_fap_vm_pointer(vm, a->nfc_trailer_buf, sizeof(*trailer), true);
        if(!dest) return false;
        memcpy(dest, trailer, sizeof(*trailer));
        result = a->nfc_trailer_buf; break;
    }
    case ArmImport_mf_classic_block_to_value: {
        const MfClassicBlock* block = arm_fap_vm_pointer(vm, p, sizeof(*block), false);
        if(!block) return false;
        int32_t value = 0;
        uint8_t addr = 0;
        bool ok = mf_classic_block_to_value(block, &value, &addr);
        if(!arm_fap_vm_write(vm, q, (uint32_t)value, 4)) return false;
        if(!arm_fap_vm_write(vm, r, addr, 1)) return false;
        result = ok ? 1 : 0; break;
    }
    case ArmImport_mf_classic_poller_sync_detect_type: {
        ArmNfcPtr* nfc = get_nfc_ptr(a, p, NfcPtrNfc); if(!nfc) return false;
        MfClassicType* type_out = arm_fap_vm_pointer(vm, q, sizeof(*type_out), true);
        if(!type_out) return false;
        result = (uint32_t)mf_classic_poller_sync_detect_type((Nfc*)nfc->native, type_out); break;
    }
    case ArmImport_mf_classic_poller_sync_auth: {
        ArmNfcPtr* nfc = get_nfc_ptr(a, p, NfcPtrNfc); if(!nfc) return false;
        uint32_t data_addr = arm_fap_vm_read(vm, vm->r[13], 4);
        MfClassicKey* key = arm_fap_vm_pointer(vm, r, sizeof(*key), false);
        MfClassicAuthContext* data = arm_fap_vm_pointer(vm, data_addr, sizeof(*data), true);
        if(!key || !data || s > MfClassicKeyTypeB || vm->error[0]) return false;
        result = (uint32_t)mf_classic_poller_sync_auth(
                     (Nfc*)nfc->native, (uint8_t)q, key, (MfClassicKeyType)s, data);
        break;
    }
    case ArmImport_mf_classic_poller_sync_read_block: {
        ArmNfcPtr* nfc = get_nfc_ptr(a, p, NfcPtrNfc); if(!nfc) return false;
        uint32_t data_addr = arm_fap_vm_read(vm, vm->r[13], 4);
        MfClassicKey* key = arm_fap_vm_pointer(vm, r, sizeof(*key), false);
        MfClassicBlock* data = arm_fap_vm_pointer(vm, data_addr, sizeof(*data), true);
        if(!key || !data || s > MfClassicKeyTypeB || vm->error[0]) return false;
        result = (uint32_t)mf_classic_poller_sync_read_block(
                     (Nfc*)nfc->native, (uint8_t)q, key, (MfClassicKeyType)s, data);
        break;
    }
    case ArmImport_mf_classic_poller_sync_read: {
        ArmNfcPtr* nfc = get_nfc_ptr(a, p, NfcPtrNfc); if(!nfc) return false;
        const MfClassicDeviceKeys* keys = arm_fap_vm_pointer(vm, q, sizeof(*keys), false);
        ArmNfcPtr* data = get_nfc_ptr(a, r, NfcPtrData);
        if(!keys || !data) return false;
        result = (uint32_t)mf_classic_poller_sync_read(
                     (Nfc*)nfc->native, keys, (MfClassicData*)data->native);
        break;
    }
    case ArmImport_mf_ultralight_get_pages_total:
        if(p >= MfUltralightTypeNum) return false;
        result = mf_ultralight_get_pages_total((MfUltralightType)p); break;
    case ArmImport_iso15693_3_get_block_count: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrData); if(!ptr) return false;
        result = iso15693_3_get_block_count((const Iso15693_3Data*)ptr->native); break;
    }
    case ArmImport_iso15693_3_get_block_size: {
        ArmNfcPtr* ptr = get_nfc_ptr(a, p, NfcPtrData); if(!ptr) return false;
        result = iso15693_3_get_block_size((const Iso15693_3Data*)ptr->native); break;
    }
    case ArmImport_canvas_clear: canvas_clear(a->canvas); break;
    case ArmImport_canvas_draw_str: {
        const char* text = arm_fap_vm_string(vm, s); if(!text) return false;
        canvas_draw_str(a->canvas, (int32_t)q, (int32_t)r, text); break;
    }
    case ArmImport_canvas_draw_box:
    case ArmImport_canvas_draw_frame: {
        uint32_t height = arm_fap_vm_read(vm, vm->r[13], 4);
        if(s > 128 || height > 64 || vm->error[0]) return false;
        if(id == ArmImport_canvas_draw_box) canvas_draw_box(a->canvas, (int32_t)q, (int32_t)r, s, height);
        else canvas_draw_frame(a->canvas, (int32_t)q, (int32_t)r, s, height);
        break;
    }
    case ArmImport_canvas_draw_line: {
        int32_t y2 = (int32_t)arm_fap_vm_read(vm, vm->r[13], 4);
        if((int32_t)s < -1024 || (int32_t)s > 1024 || y2 < -1024 || y2 > 1024 || vm->error[0]) return false;
        canvas_draw_line(a->canvas, (int32_t)q, (int32_t)r, (int32_t)s, y2); break;
    }
    case ArmImport_canvas_set_color:
        if(q > ColorXOR) return false;
        canvas_set_color(a->canvas, (Color)q); break;
    case ArmImport_canvas_set_font:
        if(q >= FontTotalNumber) return false;
        canvas_set_font(a->canvas, (Font)q); break;
    case ArmImport_canvas_draw_icon:
        if(!draw_icon(a, (int32_t)q, (int32_t)r, s)) return false;
        break;
    case ArmImport_canvas_draw_disc:
        if(s > 128) return false;
        canvas_draw_disc(a->canvas, (int32_t)q, (int32_t)r, s); break;
    case ArmImport_canvas_draw_rframe: {
        uint32_t h = arm_fap_vm_read(vm, vm->r[13], 4), radius = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        if(s > 128 || h > 64 || radius > 64 || vm->error[0]) return false;
        canvas_draw_rframe(a->canvas, (int32_t)q, (int32_t)r, s, h, radius); break;
    }
    case ArmImport_canvas_draw_str_aligned: {
        uint32_t align = arm_fap_vm_read(vm, vm->r[13], 4), text = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        const char* string = arm_fap_vm_string(vm, text);
        if(!string || s > AlignCenter || align > AlignCenter || vm->error[0]) return false;
        canvas_draw_str_aligned(a->canvas, (int32_t)q, (int32_t)r, (Align)s, (Align)align, string); break;
    }
    case ArmImport_canvas_width: result = (uint32_t)canvas_width(a->canvas); break;
    case ArmImport_canvas_height: result = (uint32_t)canvas_height(a->canvas); break;
    case ArmImport_canvas_current_font_height:
        result = (uint32_t)canvas_current_font_height(a->canvas); break;
    case ArmImport_canvas_invert_color: canvas_invert_color(a->canvas); break;
    case ArmImport_canvas_string_width: {
        const char* string = arm_fap_vm_string(vm, q);
        if(!string) return false;
        result = canvas_string_width(a->canvas, string); break;
    }
    case ArmImport_elements_button_left:
    case ArmImport_elements_button_center:
    case ArmImport_elements_button_right: {
        const char* string = arm_fap_vm_string(vm, q);
        if(!string) return false;
        if(id == ArmImport_elements_button_left) elements_button_left(a->canvas, string);
        else if(id == ArmImport_elements_button_center) elements_button_center(a->canvas, string);
        else elements_button_right(a->canvas, string);
        break;
    }
    case ArmImport_canvas_draw_dot: canvas_draw_dot(a->canvas, (int32_t)q, (int32_t)r); break;
    case ArmImport_canvas_draw_circle:
        if(s > 128) return false;
        canvas_draw_circle(a->canvas, (int32_t)q, (int32_t)r, s); break;
    case ArmImport_elements_multiline_text: {
        const char* string = arm_fap_vm_string(vm, s);
        if(!string) return false;
        elements_multiline_text(a->canvas, (int32_t)q, (int32_t)r, string); break;
    }
    case ArmImport_elements_scrollbar_pos: {
        uint32_t pos = arm_fap_vm_read(vm, vm->r[13], 4), total = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        if(s > 64 || vm->error[0]) return false;
        elements_scrollbar_pos(a->canvas, (int32_t)q, (int32_t)r, s, pos, total); break;
    }
    case ArmImport_elements_text_box: {
        uint32_t height = arm_fap_vm_read(vm, vm->r[13], 4);
        uint32_t horizontal = arm_fap_vm_read(vm, vm->r[13] + 4, 4);
        uint32_t vertical = arm_fap_vm_read(vm, vm->r[13] + 8, 4);
        uint32_t text = arm_fap_vm_read(vm, vm->r[13] + 12, 4);
        uint32_t strip = arm_fap_vm_read(vm, vm->r[13] + 16, 4);
        const char* string = arm_fap_vm_string(vm, text);
        if(!string || s > 128 || height > 64 || horizontal > AlignCenter ||
           vertical > AlignCenter || vm->error[0]) return false;
        elements_text_box(
            a->canvas, (int32_t)q, (int32_t)r, s, height, (Align)horizontal, (Align)vertical,
            string, strip != 0);
        break;
    }
    case ArmImport_furi_string_alloc:
    case ArmImport_furi_string_alloc_set_str:
    case ArmImport_furi_string_alloc_set: {
        const char* string = NULL;
        ArmString* source = NULL;
        if(id == ArmImport_furi_string_alloc_set_str) {
            string = arm_fap_vm_string(vm, p);
            if(!string) return false;
        } else if(id == ArmImport_furi_string_alloc_set) {
            source = get_string(a, p);
            if(!source) return false;
        }
        unsigned slot = MAX_STRINGS;
        for(unsigned i = 0; i < MAX_STRINGS; i++) if(!a->strings[i].native) { slot = i; break; }
        if(slot == MAX_STRINGS) return false;
        uint32_t buf = arm_fap_vm_alloc(vm, STRING_BUF_SIZE);
        if(!buf) return false;
        a->strings[slot].native = string ? furi_string_alloc_set_str(string) :
                                  source ? furi_string_alloc_set(source->native) : furi_string_alloc();
        a->strings[slot].guest_buf = buf;
        result = STRING_HANDLE + slot * 4;
        break;
    }
    case ArmImport_furi_string_free:
        furi_string_free(str->native);
        arm_fap_vm_free(vm, str->guest_buf);
        str->native = NULL;
        str->guest_buf = 0;
        break;
    case ArmImport_furi_string_reset: furi_string_reset(str->native); break;
    case ArmImport_furi_string_set_str: {
        const char* string = arm_fap_vm_string(vm, q);
        if(!string) return false;
        furi_string_set_str(str->native, string); break;
    }
    case ArmImport_furi_string_set_strn: {
        const char* bytes = arm_fap_vm_pointer(vm, q, r, false);
        if(!bytes) return false;
        furi_string_set_strn(str->native, bytes, r); break;
    }
    case ArmImport_furi_string_cat_str: {
        const char* string = arm_fap_vm_string(vm, q);
        if(!string) return false;
        furi_string_cat_str(str->native, string); break;
    }
    case ArmImport_furi_string_push_back: furi_string_push_back(str->native, (char)q); break;
    case ArmImport_furi_string_set_char:
        if(q >= furi_string_size(str->native)) return false;
        furi_string_set_char(str->native, q, (char)r); break;
    case ArmImport_furi_string_get_char:
        if(q >= furi_string_size(str->native)) return false;
        result = (uint8_t)furi_string_get_char(str->native, q); break;
    case ArmImport_furi_string_get_cstr: {
        const char* cstr = furi_string_get_cstr(str->native);
        char* dest = arm_fap_vm_pointer(vm, str->guest_buf, STRING_BUF_SIZE, true);
        if(!dest) return false;
        size_t n = strlen(cstr);
        if(n > STRING_BUF_SIZE - 1) n = STRING_BUF_SIZE - 1;
        memcpy(dest, cstr, n);
        dest[n] = 0;
        result = str->guest_buf; break;
    }
    case ArmImport_furi_string_size: result = (uint32_t)furi_string_size(str->native); break;
    case ArmImport_furi_string_empty: result = furi_string_empty(str->native) ? 1 : 0; break;
    case ArmImport_furi_string_equal_str: {
        const char* string = arm_fap_vm_string(vm, q);
        if(!string) return false;
        result = furi_string_equal_str(str->native, string) ? 1 : 0; break;
    }
    case ArmImport_furi_string_cmp_str: {
        const char* string = arm_fap_vm_string(vm, q);
        if(!string) return false;
        result = (uint32_t)furi_string_cmp_str(str->native, string); break;
    }
    case ArmImport_furi_string_start_with_str: {
        const char* string = arm_fap_vm_string(vm, q);
        if(!string) return false;
        result = furi_string_start_with_str(str->native, string) ? 1 : 0; break;
    }
    case ArmImport_furi_string_reserve: furi_string_reserve(str->native, q); break;
    case ArmImport_furi_string_move: {
        ArmString* source = get_string(a, q); if(!source) return false;
        furi_string_move(str->native, source->native); break;
    }
    case ArmImport_furi_string_set: {
        ArmString* source = get_string(a, q); if(!source) return false;
        furi_string_set(str->native, source->native); break;
    }
    case ArmImport_furi_string_set_n: {
        ArmString* source = get_string(a, q); if(!source) return false;
        furi_string_set_n(str->native, source->native, r, s); break;
    }
    case ArmImport_furi_string_cat: {
        ArmString* source = get_string(a, q); if(!source) return false;
        furi_string_cat(str->native, source->native); break;
    }
    case ArmImport_furi_string_cmp: {
        ArmString* other = get_string(a, q); if(!other) return false;
        result = (uint32_t)furi_string_cmp(str->native, other->native); break;
    }
    case ArmImport_furi_string_cmpi_str: {
        const char* string = arm_fap_vm_string(vm, q);
        if(!string) return false;
        result = (uint32_t)furi_string_cmpi_str(str->native, string); break;
    }
    case ArmImport_furi_string_search_str: {
        const char* needle = arm_fap_vm_string(vm, q);
        if(!needle) return false;
        result = (uint32_t)furi_string_search_str(str->native, needle, r); break;
    }
    case ArmImport_furi_string_search_char:
        result = (uint32_t)furi_string_search_char(str->native, (char)q, r); break;
    case ArmImport_furi_string_search_rchar:
        result = (uint32_t)furi_string_search_rchar(str->native, (char)q, r); break;
    case ArmImport_furi_string_equal: {
        ArmString* other = get_string(a, q); if(!other) return false;
        result = furi_string_equal(str->native, other->native) ? 1 : 0; break;
    }
    case ArmImport_furi_string_replace_str: {
        uint32_t start = arm_fap_vm_read(vm, vm->r[13], 4);
        const char* needle = arm_fap_vm_string(vm, q);
        const char* replace = arm_fap_vm_string(vm, r);
        if(!needle || !replace || vm->error[0]) return false;
        result = (uint32_t)furi_string_replace_str(str->native, needle, replace, start); break;
    }
    case ArmImport_furi_string_replace_all_str: {
        const char* needle = arm_fap_vm_string(vm, q);
        const char* replace = arm_fap_vm_string(vm, r);
        if(!needle || !replace) return false;
        furi_string_replace_all_str(str->native, needle, replace); break;
    }
    case ArmImport_furi_string_left: furi_string_left(str->native, q); break;
    case ArmImport_furi_string_right: furi_string_right(str->native, q); break;
    case ArmImport_furi_string_trim: {
        const char* chars = arm_fap_vm_string(vm, q);
        if(!chars) return false;
        furi_string_trim(str->native, chars); break;
    }
    case ArmImport_furi_string_utf8_length:
        result = (uint32_t)furi_string_utf8_length(str->native); break;
    case ArmImport_storage_file_alloc:
        if(!has_storage(a, p)) return false;
        for(unsigned i = 0; i < MAX_FILES; i++) if(!a->files[i].native) {
            a->files[i].native = storage_file_alloc(a->storage);
            result = FILE_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_storage_file_free:
        if(file->is_dir) return false;
        storage_file_free(file->native); memset(file, 0, sizeof(*file)); break;
    case ArmImport_storage_file_open: {
        const char* path = arm_fap_vm_string(vm, q);
        if(!path || !r || r > FSAM_READ_WRITE || s > FSOM_CREATE_ALWAYS) return false;
        file->is_dir = false;
        result = storage_file_open(file->native, path, (FS_AccessMode)r, (FS_OpenMode)s) ? 1 : 0;
        break;
    }
    case ArmImport_storage_file_close:
        result = storage_file_close(file->native) ? 1 : 0; break;
    case ArmImport_storage_file_read: {
        uint8_t* buffer = r ? arm_fap_vm_pointer(vm, q, r, true) : NULL;
        if((r && !buffer) || vm->error[0]) return false;
        result = (uint32_t)storage_file_read(file->native, buffer, r);
        break;
    }
    case ArmImport_storage_file_write: {
        const uint8_t* buffer = r ? arm_fap_vm_pointer(vm, q, r, false) : NULL;
        if((r && !buffer) || vm->error[0]) return false;
        result = (uint32_t)storage_file_write(file->native, buffer, r);
        break;
    }
    case ArmImport_storage_file_seek:
        result = storage_file_seek(file->native, q, r != 0) ? 1 : 0; break;
    case ArmImport_storage_file_tell:
    case ArmImport_storage_file_size: {
        uint64_t value = id == ArmImport_storage_file_size ? storage_file_size(file->native) :
                                                             storage_file_tell(file->native);
        result = (uint32_t)value;
        vm->r[1] = (uint32_t)(value >> 32);
        break;
    }
    case ArmImport_storage_file_get_error:
        result = (uint32_t)storage_file_get_error(file->native); break;
    case ArmImport_storage_file_get_error_desc: {
        const char* desc = storage_file_get_error_desc(file->native);
        if(!desc) return false;
        if(!a->fs_scratch) a->fs_scratch = arm_fap_vm_alloc(vm, FS_SCRATCH_SIZE);
        if(!a->fs_scratch) return false;
        char* dest = arm_fap_vm_pointer(vm, a->fs_scratch, FS_SCRATCH_SIZE, true);
        if(!dest) return false;
        size_t n = strlen(desc); if(n > FS_SCRATCH_SIZE - 1) n = FS_SCRATCH_SIZE - 1;
        memcpy(dest, desc, n); dest[n] = 0;
        result = a->fs_scratch; break;
    }
    case ArmImport_storage_dir_open: {
        const char* path = arm_fap_vm_string(vm, q);
        if(!path) return false;
        file->is_dir = true;
        result = storage_dir_open(file->native, path) ? 1 : 0;
        break;
    }
    case ArmImport_storage_dir_close:
        result = storage_dir_close(file->native) ? 1 : 0;
        file->is_dir = false;
        break;
    case ArmImport_storage_dir_read: {
        if(s > 0xffffu) return false;
        FileInfo* info = q ? arm_fap_vm_pointer(vm, q, sizeof(FileInfo), true) : NULL;
        char* name = r ? arm_fap_vm_pointer(vm, r, s ? s : 1, true) : NULL;
        if((q && !info) || (r && !name) || vm->error[0]) return false;
        result = storage_dir_read(file->native, info, name, (uint16_t)s) ? 1 : 0;
        break;
    }
    case ArmImport_storage_file_exists:
    case ArmImport_storage_dir_exists:
    case ArmImport_storage_common_exists: {
        const char* path = arm_fap_vm_string(vm, q);
        if(!path) return false;
        if(id == ArmImport_storage_file_exists) result = storage_file_exists(a->storage, path) ? 1 : 0;
        else if(id == ArmImport_storage_dir_exists) result = storage_dir_exists(a->storage, path) ? 1 : 0;
        else result = storage_common_exists(a->storage, path) ? 1 : 0;
        break;
    }
    case ArmImport_storage_common_copy:
    case ArmImport_storage_common_migrate:
    case ArmImport_storage_common_rename: {
        const char* source = arm_fap_vm_string(vm, q);
        const char* dest = arm_fap_vm_string(vm, r);
        if(!source || !dest) return false;
        FS_Error error;
        if(id == ArmImport_storage_common_copy)
            error = storage_common_copy(a->storage, source, dest);
        else if(id == ArmImport_storage_common_rename)
            error = storage_common_rename(a->storage, source, dest);
        else
            error = storage_common_migrate(a->storage, source, dest);
        result = (uint32_t)error;
        break;
    }
    case ArmImport_storage_common_mkdir:
    case ArmImport_storage_common_remove: {
        const char* path = arm_fap_vm_string(vm, q);
        if(!path) return false;
        result = (uint32_t)(id == ArmImport_storage_common_mkdir ?
            storage_common_mkdir(a->storage, path) : storage_common_remove(a->storage, path));
        break;
    }
    case ArmImport_storage_common_stat: {
        const char* path = arm_fap_vm_string(vm, q);
        FileInfo* info = r ? arm_fap_vm_pointer(vm, r, sizeof(FileInfo), true) : NULL;
        if(!path || (r && !info) || vm->error[0]) return false;
        result = (uint32_t)storage_common_stat(a->storage, path, info);
        break;
    }
    case ArmImport_storage_sd_status: result = (uint32_t)storage_sd_status(a->storage); break;
    case ArmImport_storage_simply_mkdir:
    case ArmImport_storage_simply_remove:
    case ArmImport_storage_simply_remove_recursive: {
        const char* path = arm_fap_vm_string(vm, q);
        if(!path) return false;
        if(id == ArmImport_storage_simply_mkdir)
            result = storage_simply_mkdir(a->storage, path) ? 1 : 0;
        else if(id == ArmImport_storage_simply_remove)
            result = storage_simply_remove(a->storage, path) ? 1 : 0;
        else
            result = storage_simply_remove_recursive(a->storage, path) ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_string_alloc:
    case ArmImport_flipper_format_file_alloc:
    case ArmImport_flipper_format_buffered_file_alloc: {
        if(id != ArmImport_flipper_format_string_alloc && !a->storage) {
            fault(a, "Storage record not open"); return false;
        }
        FlipperFormat* native = id == ArmImport_flipper_format_string_alloc ?
            flipper_format_string_alloc() :
            id == ArmImport_flipper_format_file_alloc ?
                flipper_format_file_alloc(a->storage) :
                flipper_format_buffered_file_alloc(a->storage);
        result = wrap_format(a, native);
        if(!result) { if(native) flipper_format_free(native); return false; }
        break;
    }
    case ArmImport_flipper_format_free:
        if(format->raw_stream) {
            ArmStream* raw = get_stream(a, format->raw_stream);
            if(raw) memset(raw, 0, sizeof(*raw)); /* borrowed: the format owns it */
        }
        flipper_format_free(format->native);
        memset(format, 0, sizeof(*format));
        break;
    case ArmImport_flipper_format_file_close:
        result = flipper_format_file_close(format->native) ? 1 : 0; break;
    case ArmImport_flipper_format_file_open_always:
    case ArmImport_flipper_format_file_open_append:
    case ArmImport_flipper_format_file_open_existing:
    case ArmImport_flipper_format_file_open_new:
    case ArmImport_flipper_format_buffered_file_open_existing: {
        const char* path = arm_fap_vm_string(vm, q);
        if(!path) return false;
        bool ok;
        if(id == ArmImport_flipper_format_file_open_always)
            ok = flipper_format_file_open_always(format->native, path);
        else if(id == ArmImport_flipper_format_file_open_append)
            ok = flipper_format_file_open_append(format->native, path);
        else if(id == ArmImport_flipper_format_file_open_existing)
            ok = flipper_format_file_open_existing(format->native, path);
        else if(id == ArmImport_flipper_format_file_open_new)
            ok = flipper_format_file_open_new(format->native, path);
        else
            ok = flipper_format_buffered_file_open_existing(format->native, path);
        result = ok ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_rewind:
        result = flipper_format_rewind(format->native) ? 1 : 0; break;
    case ArmImport_flipper_format_seek_to_end:
        result = flipper_format_seek_to_end(format->native) ? 1 : 0; break;
    case ArmImport_flipper_format_get_raw_stream:
        if(!format->raw_stream) {
            format->raw_stream = wrap_stream(a, flipper_format_get_raw_stream(format->native), false);
            if(!format->raw_stream) return false;
        }
        result = format->raw_stream; break;
    case ArmImport_flipper_format_get_value_count: {
        const char* key = arm_fap_vm_string(vm, q);
        uint32_t* count = arm_fap_vm_pointer(vm, r, 4, true);
        if(!key || !count) return false;
        result = flipper_format_get_value_count(format->native, key, count) ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_read_bool:
    case ArmImport_flipper_format_read_float:
    case ArmImport_flipper_format_read_hex:
    case ArmImport_flipper_format_read_int32:
    case ArmImport_flipper_format_read_uint32: {
        const char* key = arm_fap_vm_string(vm, q);
        size_t element = (id == ArmImport_flipper_format_read_hex ||
                          id == ArmImport_flipper_format_read_bool) ? 1 : 4;
        void* data = s ? arm_fap_vm_pointer(vm, r, (size_t)s * element, true) : NULL;
        if(!key || (s && !data) || vm->error[0]) return false;
        bool ok;
        if(id == ArmImport_flipper_format_read_bool)
            ok = flipper_format_read_bool(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_read_float)
            ok = flipper_format_read_float(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_read_hex)
            ok = flipper_format_read_hex(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_read_int32)
            ok = flipper_format_read_int32(format->native, key, data, (uint16_t)s);
        else
            ok = flipper_format_read_uint32(format->native, key, data, (uint16_t)s);
        result = ok ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_write_bool:
    case ArmImport_flipper_format_write_float:
    case ArmImport_flipper_format_write_hex:
    case ArmImport_flipper_format_write_uint32:
    case ArmImport_flipper_format_update_hex:
    case ArmImport_flipper_format_update_uint32:
    case ArmImport_flipper_format_insert_or_update_bool:
    case ArmImport_flipper_format_insert_or_update_float:
    case ArmImport_flipper_format_insert_or_update_hex:
    case ArmImport_flipper_format_insert_or_update_uint32: {
        const char* key = arm_fap_vm_string(vm, q);
        size_t element = (id == ArmImport_flipper_format_write_hex ||
                          id == ArmImport_flipper_format_update_hex ||
                          id == ArmImport_flipper_format_insert_or_update_hex ||
                          id == ArmImport_flipper_format_write_bool ||
                          id == ArmImport_flipper_format_insert_or_update_bool) ? 1 : 4;
        const void* data = s ? arm_fap_vm_pointer(vm, r, (size_t)s * element, false) : NULL;
        if(!key || (s && !data) || vm->error[0]) return false;
        bool ok;
        if(id == ArmImport_flipper_format_write_bool)
            ok = flipper_format_write_bool(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_write_float)
            ok = flipper_format_write_float(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_write_hex)
            ok = flipper_format_write_hex(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_write_uint32)
            ok = flipper_format_write_uint32(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_update_hex)
            ok = flipper_format_update_hex(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_update_uint32)
            ok = flipper_format_update_uint32(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_insert_or_update_bool)
            ok = flipper_format_insert_or_update_bool(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_insert_or_update_float)
            ok = flipper_format_insert_or_update_float(format->native, key, data, (uint16_t)s);
        else if(id == ArmImport_flipper_format_insert_or_update_hex)
            ok = flipper_format_insert_or_update_hex(format->native, key, data, (uint16_t)s);
        else
            ok = flipper_format_insert_or_update_uint32(format->native, key, data, (uint16_t)s);
        result = ok ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_read_header: {
        ArmString* filetype = get_string(a, q); if(!filetype) return false;
        uint32_t* version = arm_fap_vm_pointer(vm, r, 4, true);
        if(!version) return false;
        result = flipper_format_read_header(format->native, filetype->native, version) ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_read_string: {
        const char* key = arm_fap_vm_string(vm, q);
        ArmString* data = get_string(a, r);
        if(!key || !data) return false;
        result = flipper_format_read_string(format->native, key, data->native) ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_write_string: {
        const char* key = arm_fap_vm_string(vm, q);
        ArmString* data = get_string(a, r);
        if(!key || !data) return false;
        result = flipper_format_write_string(format->native, key, data->native) ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_write_string_cstr:
    case ArmImport_flipper_format_insert_or_update_string_cstr: {
        const char* key = arm_fap_vm_string(vm, q);
        const char* data = arm_fap_vm_string(vm, r);
        if(!key || !data) return false;
        result = (id == ArmImport_flipper_format_write_string_cstr ?
            flipper_format_write_string_cstr(format->native, key, data) :
            flipper_format_insert_or_update_string_cstr(format->native, key, data)) ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_write_header_cstr: {
        const char* filetype = arm_fap_vm_string(vm, q);
        if(!filetype) return false;
        result = flipper_format_write_header_cstr(format->native, filetype, r) ? 1 : 0;
        break;
    }
    case ArmImport_flipper_format_write_comment_cstr: {
        const char* comment = arm_fap_vm_string(vm, q);
        if(!comment) return false;
        result = flipper_format_write_comment_cstr(format->native, comment) ? 1 : 0;
        break;
    }
    case ArmImport_stream_clean: stream_clean(stream->native); break;
    case ArmImport_stream_free:
        if(!stream->owned) return false;
        stream_free(stream->native);
        memset(stream, 0, sizeof(*stream));
        break;
    case ArmImport_stream_eof: result = stream_eof(stream->native) ? 1 : 0; break;
    case ArmImport_stream_rewind: result = stream_rewind(stream->native) ? 1 : 0; break;
    case ArmImport_stream_size: result = (uint32_t)stream_size(stream->native); break;
    case ArmImport_stream_tell: result = (uint32_t)stream_tell(stream->native); break;
    case ArmImport_stream_seek:
        if(r > StreamOffsetFromEnd) return false;
        result = stream_seek(stream->native, (int32_t)q, (StreamOffset)r) ? 1 : 0; break;
    case ArmImport_stream_seek_to_char:
        if(r > StreamDirectionBackward) return false;
        result = stream_seek_to_char(stream->native, (char)q, (StreamDirection)r) ? 1 : 0; break;
    case ArmImport_stream_delete: result = stream_delete(stream->native, q) ? 1 : 0; break;
    case ArmImport_stream_copy: {
        ArmStream* destination = get_stream(a, q); if(!destination) return false;
        result = (uint32_t)stream_copy(stream->native, destination->native, r);
        break;
    }
    case ArmImport_stream_read_line: {
        ArmString* line = get_string(a, q); if(!line) return false;
        result = stream_read_line(stream->native, line->native) ? 1 : 0;
        break;
    }
    case ArmImport_stream_write_char:
        result = (uint32_t)stream_write_char(stream->native, (char)q); break;
    case ArmImport_stream_write: {
        const uint8_t* data = r ? arm_fap_vm_pointer(vm, q, r, false) : NULL;
        if((r && !data) || vm->error[0]) return false;
        result = (uint32_t)stream_write(stream->native, data, r); break;
    }
    case ArmImport_stream_read: {
        uint8_t* data = r ? arm_fap_vm_pointer(vm, q, r, true) : NULL;
        if((r && !data) || vm->error[0]) return false;
        result = (uint32_t)stream_read(stream->native, data, r); break;
    }
    case ArmImport_stream_insert: {
        const uint8_t* data = r ? arm_fap_vm_pointer(vm, q, r, false) : NULL;
        if((r && !data) || vm->error[0]) return false;
        result = stream_insert(stream->native, data, r) ? 1 : 0; break;
    }
    case ArmImport_furi_delay_tick: {
        if(p > 60000) return false;
        uint32_t start = guest_tick(), left = p;
        do {
            GUI_CALL(furi_delay_ms(MIN(left, 50)));
            uint32_t elapsed = guest_tick() - start;
            left = elapsed >= p ? 0 : p - elapsed;
        } while(left && !vm->error[0]);
        if(guest_tick() != start) vm->yields++;
        break;
    }
    case ArmImport_furi_kernel_get_tick_frequency: result = 1000; break;
    case ArmImport_furi_log_set_level:
        if(p > FuriLogLevelTrace) return false;
        furi_log_set_level((FuriLogLevel)p);
        break;
    case ArmImport_furi_log_print_format: {
        const char* tag = arm_fap_vm_string(vm, q);
        const char* format = arm_fap_vm_string(vm, r);
        if(!tag || !format || p > FuriLogLevelTrace) return false;
        log_guest_message(a, (FuriLogLevel)p, tag, format);
        break;
    }
    case ArmImport_furi_mutex_alloc:
        if(p > FuriMutexTypeRecursive) return false;
        for(unsigned i = 0; i < MAX_MUTEXES; i++) if(!a->mutexes[i].native) {
            a->mutexes[i].native = furi_mutex_alloc((FuriMutexType)p);
            result = MUTEX_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_furi_mutex_free:
        if(mutex->busy) return false;
        furi_mutex_free(mutex->native); memset(mutex, 0, sizeof(*mutex)); break;
    case ArmImport_furi_mutex_acquire:
        mutex->busy++;
        result = (uint32_t)status_wait(a, q, mutex_acquire_wait, mutex->native);
        mutex->busy--;
        break;
    case ArmImport_furi_mutex_release:
        result = (uint32_t)furi_mutex_release(mutex->native); break;
    case ArmImport_furi_semaphore_alloc:
        if(!p || p > 32 || q > p) return false;
        for(unsigned i = 0; i < MAX_SEMAPHORES; i++) if(!a->semaphores[i].native) {
            a->semaphores[i].native = furi_semaphore_alloc(p, q);
            result = SEMAPHORE_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_furi_semaphore_free:
        if(semaphore->busy) return false;
        furi_semaphore_free(semaphore->native); memset(semaphore, 0, sizeof(*semaphore)); break;
    case ArmImport_furi_semaphore_acquire:
        semaphore->busy++;
        result = (uint32_t)status_wait(a, q, semaphore_acquire_wait, semaphore->native);
        semaphore->busy--;
        break;
    case ArmImport_furi_semaphore_release:
        result = (uint32_t)furi_semaphore_release(semaphore->native); break;
    case ArmImport_furi_event_flag_alloc:
        for(unsigned i = 0; i < MAX_EVENT_FLAGS; i++) if(!a->event_flags[i].native) {
            a->event_flags[i].native = furi_event_flag_alloc();
            result = EVENT_FLAG_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_furi_event_flag_free:
        furi_event_flag_free(event_flag->native);
        memset(event_flag, 0, sizeof(*event_flag)); break;
    case ArmImport_furi_event_flag_set:
        if(q & ~0x00ffffffu) return false;
        result = furi_event_flag_set(event_flag->native, q); break;
    case ArmImport_furi_event_flag_wait:
        if((q & ~0x00ffffffu) || (r & ~(FuriFlagWaitAll | FuriFlagNoClear))) return false;
        result = event_flag_wait_call(a, event_flag->native, q, r, s);
        break;
    case ArmImport_furi_hal_bt_extra_beacon_is_active:
        result = furi_hal_bt_extra_beacon_is_active() ? 1 : 0; break;
    case ArmImport_furi_hal_bt_extra_beacon_set_config: {
        if(!arm_fap_vm_pointer(vm, p, ARM_EXTRA_BEACON_CONFIG_SIZE, false)) return false;
        GapExtraBeaconConfig config;
        config.min_adv_interval_ms = (uint16_t)arm_fap_vm_read(vm, p, 2);
        config.max_adv_interval_ms = (uint16_t)arm_fap_vm_read(vm, p + 2, 2);
        config.adv_channel_map = (GapAdvChannelMap)arm_fap_vm_read(vm, p + 4, 1);
        config.adv_power_level = (GapAdvPowerLevelInd)arm_fap_vm_read(vm, p + 5, 1);
        config.address_type = (GapAddressType)arm_fap_vm_read(vm, p + 6, 1);
        for(unsigned i = 0; i < EXTRA_BEACON_MAC_ADDR_SIZE; i++)
            config.address[i] = (uint8_t)arm_fap_vm_read(vm, p + 7 + i, 1);
        if(vm->error[0]) return false;
        result = furi_hal_bt_extra_beacon_set_config(&config) ? 1 : 0;
        break;
    }
    case ArmImport_furi_hal_bt_extra_beacon_set_data: {
        if(r > EXTRA_BEACON_MAX_DATA_SIZE) return false;
        const uint8_t* data = r ? arm_fap_vm_pointer(vm, p, r, false) : NULL;
        if((r && !data) || vm->error[0]) return false;
        result = furi_hal_bt_extra_beacon_set_data(data, (uint8_t)r) ? 1 : 0;
        break;
    }
    case ArmImport_furi_hal_bt_extra_beacon_start:
        result = furi_hal_bt_extra_beacon_start() ? 1 : 0; break;
    case ArmImport_furi_hal_bt_extra_beacon_stop:
        result = furi_hal_bt_extra_beacon_stop() ? 1 : 0; break;
    case ArmImport_furi_hal_bt_start_advertising: furi_hal_bt_start_advertising(); break;
    case ArmImport_furi_hal_bt_stop_advertising: furi_hal_bt_stop_advertising(); break;
    case ArmImport_furi_hal_crypto_enclave_ensure_key:
        result = furi_hal_crypto_enclave_ensure_key((uint8_t)p) ? 1 : 0; break;
    case ArmImport_furi_hal_crypto_enclave_load_key: {
        const uint8_t* iv = q ? arm_fap_vm_pointer(vm, q, CRYPTO_IV_SIZE, false) : NULL;
        if(!iv) return false;
        result = furi_hal_crypto_enclave_load_key((uint8_t)p, iv) ? 1 : 0;
        break;
    }
    case ArmImport_furi_hal_crypto_enclave_unload_key:
        result = furi_hal_crypto_enclave_unload_key((uint8_t)p) ? 1 : 0; break;
    case ArmImport_furi_hal_crypto_encrypt:
    case ArmImport_furi_hal_crypto_decrypt: {
        if(!r) { result = 0; break; }
        const uint8_t* input = arm_fap_vm_pointer(vm, p, r, false);
        uint8_t* output = arm_fap_vm_pointer(vm, q, r, true);
        if(!input || !output || vm->error[0]) return false;
        result = (id == ArmImport_furi_hal_crypto_encrypt ?
            furi_hal_crypto_encrypt(input, output, r) :
            furi_hal_crypto_decrypt(input, output, r)) ? 1 : 0;
        break;
    }
    case ArmImport_furi_hal_hid_is_connected:
        result = furi_hal_hid_is_connected() ? 1 : 0; break;
    case ArmImport_furi_hal_hid_kb_press:
        result = furi_hal_hid_kb_press((uint16_t)p) ? 1 : 0; break;
    case ArmImport_furi_hal_hid_kb_release:
        result = furi_hal_hid_kb_release((uint16_t)p) ? 1 : 0; break;
    case ArmImport_furi_hal_hid_kb_release_all:
        result = furi_hal_hid_kb_release_all() ? 1 : 0; break;
    case ArmImport_furi_hal_infrared_detect_tx_output:
        result = (uint32_t)furi_hal_infrared_detect_tx_output(); break;
    case ArmImport_furi_hal_infrared_set_tx_output:
        if(p >= FuriHalInfraredTxPinMax) return false;
        furi_hal_infrared_set_tx_output((FuriHalInfraredTxPin)p); break;
    case ArmImport_furi_hal_nfc_field_detect_start:
        result = (uint32_t)furi_hal_nfc_field_detect_start(); break;
    case ArmImport_furi_hal_nfc_field_detect_stop:
        result = (uint32_t)furi_hal_nfc_field_detect_stop(); break;
    case ArmImport_furi_hal_nfc_field_is_present:
        result = furi_hal_nfc_field_is_present() ? 1 : 0; break;
    case ArmImport_furi_hal_power_disable_otg: furi_hal_power_disable_otg(); break;
    case ArmImport_furi_hal_power_enable_otg:
        result = furi_hal_power_enable_otg() ? 1 : 0; break;
    case ArmImport_furi_hal_power_get_battery_full_capacity:
        result = furi_hal_power_get_battery_full_capacity(); break;
    case ArmImport_furi_hal_power_get_battery_remaining_capacity:
        result = furi_hal_power_get_battery_remaining_capacity(); break;
    case ArmImport_furi_hal_power_insomnia_enter:
        if(a->power_insomnia >= 255) return false;
        a->power_insomnia++;
        furi_hal_power_insomnia_enter(); break;
    case ArmImport_furi_hal_power_insomnia_exit:
        if(!a->power_insomnia) return false; /* the native counter would abort */
        a->power_insomnia--;
        furi_hal_power_insomnia_exit(); break;
    case ArmImport_furi_hal_power_is_otg_enabled:
        result = furi_hal_power_is_otg_enabled() ? 1 : 0; break;
    case ArmImport_furi_hal_power_suppress_charge_enter:
        furi_hal_power_suppress_charge_enter(); break;
    case ArmImport_furi_hal_power_suppress_charge_exit:
        furi_hal_power_suppress_charge_exit(); break;
    case ArmImport_furi_hal_random_fill_buf: {
        uint8_t* buffer = p ? arm_fap_vm_pointer(vm, p, q, true) : NULL;
        if((p && !buffer) || vm->error[0]) return false;
        furi_hal_random_fill_buf(buffer, q);
        break;
    }
    case ArmImport_furi_hal_rfid_field_detect_start: furi_hal_rfid_field_detect_start(); break;
    case ArmImport_furi_hal_rfid_field_detect_stop: furi_hal_rfid_field_detect_stop(); break;
    case ArmImport_furi_hal_rfid_field_is_present: {
        uint32_t* frequency = q ? arm_fap_vm_pointer(vm, q, 4, true) : NULL;
        if((q && !frequency) || vm->error[0]) return false;
        result = furi_hal_rfid_field_is_present(frequency) ? 1 : 0;
        break;
    }
    case ArmImport_furi_hal_rtc_get_datetime: {
        DateTime* datetime = arm_fap_vm_pointer(vm, p, sizeof(DateTime), true);
        if(!datetime) return false;
        furi_hal_rtc_get_datetime(datetime);
        break;
    }
    case ArmImport_furi_hal_rtc_get_locale_units:
        result = (uint32_t)furi_hal_rtc_get_locale_units(); break;
    case ArmImport_furi_hal_rtc_get_timestamp: result = furi_hal_rtc_get_timestamp(); break;
    case ArmImport_furi_hal_rtc_is_flag_set:
        if(p & ~0xffu) return false;
        result = furi_hal_rtc_is_flag_set((FuriHalRtcFlag)p) ? 1 : 0; break;
    case ArmImport_furi_hal_speaker_acquire:
        result = speaker_acquire_wait(a, p) ? 1 : 0; break;
    case ArmImport_furi_hal_speaker_is_mine:
        result = furi_hal_speaker_is_mine() ? 1 : 0; break;
    case ArmImport_furi_hal_speaker_release:
        if(!furi_hal_speaker_is_mine()) return false;
        furi_hal_speaker_release(); break;
    case ArmImport_furi_hal_speaker_stop:
        if(!furi_hal_speaker_is_mine()) return false;
        furi_hal_speaker_stop(); break;
    case ArmImport_furi_hal_usb_unlock: furi_hal_usb_unlock(); break;
    case ArmImport_furi_hal_version_uid: {
        const uint8_t* uid = furi_hal_version_uid();
        size_t size = furi_hal_version_uid_size();
        if(!uid || !size || size > HAL_UID_BUF_SIZE) return false;
        if(!a->hal_uid_buf) a->hal_uid_buf = arm_fap_vm_alloc(vm, HAL_UID_BUF_SIZE);
        if(!a->hal_uid_buf) return false;
        uint8_t* dest = arm_fap_vm_pointer(vm, a->hal_uid_buf, HAL_UID_BUF_SIZE, true);
        if(!dest) return false;
        memcpy(dest, uid, size);
        result = a->hal_uid_buf; break;
    }
    case ArmImport_furi_hal_version_uid_size:
        result = (uint32_t)furi_hal_version_uid_size(); break;
    case ArmImport_subghz_block_generic_deserialize:
    case ArmImport_subghz_block_generic_deserialize_check_count_bit: {
        SubGhzBlockGeneric* instance = arm_fap_vm_pointer(vm, p, sizeof(*instance), true);
        ArmFormat* format = get_format(a, q);
        if(!instance || !format) return false;
        if(id == ArmImport_subghz_block_generic_deserialize)
            result = (uint32_t)subghz_block_generic_deserialize(instance, format->native);
        else
            result = (uint32_t)subghz_block_generic_deserialize_check_count_bit(
                instance, format->native, (uint16_t)r);
        break;
    }
    case ArmImport_subghz_devices_init:
        if(!subghz_device_registry_is_valid()) {
            subghz_devices_init();
            a->subghz_devices_owned = true;
        }
        break;
    case ArmImport_subghz_devices_deinit:
        if(!a->subghz_devices_owned) break;
        for(unsigned i = 0; i < MAX_SUBGHZ_DEVICES; i++) if(a->subghz_devices[i].native) return false;
        subghz_devices_deinit();
        a->subghz_devices_owned = false;
        break;
    case ArmImport_subghz_devices_get_by_name: {
        const char* name = arm_fap_vm_string(vm, p);
        if(!name) return false;
        if(!subghz_device_registry_is_valid()) { result = 0; break; }
        result = wrap_subghz_device(a, subghz_devices_get_by_name(name));
        break;
    }
    case ArmImport_subghz_devices_begin:
        result = subghz_devices_begin(sg_device->native) ? 1 : 0; break;
    case ArmImport_subghz_devices_end: subghz_devices_end(sg_device->native); break;
    case ArmImport_subghz_devices_flush_rx: subghz_devices_flush_rx(sg_device->native); break;
    case ArmImport_subghz_devices_get_rssi: {
        float rssi = subghz_devices_get_rssi(sg_device->native);
        memcpy(&result, &rssi, sizeof(result));
        break;
    }
    case ArmImport_subghz_devices_idle: subghz_devices_idle(sg_device->native); break;
    case ArmImport_subghz_devices_is_connect:
        result = subghz_devices_is_connect(sg_device->native) ? 1 : 0; break;
    case ArmImport_subghz_devices_is_frequency_valid:
        result = furi_hal_subghz_is_frequency_valid(q) ? 1 : 0; break;
    case ArmImport_subghz_devices_load_preset: {
        if(!r || r > FuriHalSubGhzPresetCustom) return false;
        uint8_t preset_data[SUBGHZ_PRESET_BUF_SIZE];
        uint8_t* data = NULL;
        if(r == FuriHalSubGhzPresetCustom) {
            /* Copy the guest register list and its 8-byte PA table into a
             * native buffer: the driver scans for the terminator itself. */
            size_t i = 0;
            for(; i < SUBGHZ_PRESET_BUF_SIZE - 10; i++) {
                preset_data[i] = (uint8_t)arm_fap_vm_read(vm, s + i, 1);
                if(vm->error[0]) return false;
                if(!preset_data[i]) break;
            }
            if(i >= SUBGHZ_PRESET_BUF_SIZE - 10) return false;
            for(size_t k = i + 1; k <= i + 9; k++)
                preset_data[k] = (uint8_t)arm_fap_vm_read(vm, s + k, 1);
            if(vm->error[0]) return false;
            data = preset_data;
        }
        subghz_devices_load_preset(sg_device->native, (FuriHalSubGhzPreset)r, data);
        break;
    }
    case ArmImport_subghz_devices_reset: subghz_devices_reset(sg_device->native); break;
    case ArmImport_subghz_devices_set_frequency:
        if(!furi_hal_subghz_is_frequency_valid(q)) { result = 0; break; }
        result = subghz_devices_set_frequency(sg_device->native, q);
        break;
    case ArmImport_subghz_devices_set_rx: subghz_devices_set_rx(sg_device->native); break;
    case ArmImport_subghz_devices_set_tx:
        result = subghz_devices_set_tx(sg_device->native) ? 1 : 0; break;
    case ArmImport_subghz_devices_sleep: subghz_devices_sleep(sg_device->native); break;
    case ArmImport_subghz_environment_alloc: {
        SubGhzEnvironment* env = subghz_environment_alloc();
        if(env) {
            /* Guest code cannot import the registry variable, so wire the
             * built-in protocol registry: receiver/transmitter alloc need it. */
            subghz_environment_set_protocol_registry(env, &subghz_protocol_registry);
        }
        result = wrap_subghz_env(a, env);
        if(!result) { if(env) subghz_environment_free(env); return false; }
        break;
    }
    case ArmImport_subghz_environment_free:
        subghz_environment_free(sg_env->native);
        memset(sg_env, 0, sizeof(*sg_env));
        break;
    case ArmImport_subghz_environment_get_protocol_name_registry:
        result = subghz_name_handle(
            a, subghz_environment_get_protocol_name_registry(sg_env->native, q));
        break;
    case ArmImport_subghz_environment_load_keystore: {
        const char* path = arm_fap_vm_string(vm, q);
        if(!path) return false;
        result = subghz_environment_load_keystore(sg_env->native, path) ? 1 : 0;
        break;
    }
    case ArmImport_subghz_keystore_raw_get_data: {
        const char* path = arm_fap_vm_string(vm, p);
        uint8_t* data = s ? arm_fap_vm_pointer(vm, r, s, true) : NULL;
        if(!path || (s && !data) || vm->error[0]) return false;
        result = subghz_keystore_raw_get_data(path, q, data, s) ? 1 : 0;
        break;
    }
    case ArmImport_subghz_protocol_blocks_add_bit: {
        SubGhzBlockDecoder* decoder = arm_fap_vm_pointer(vm, p, sizeof(*decoder), true);
        if(!decoder) return false;
        subghz_protocol_blocks_add_bit(decoder, (uint8_t)q);
        break;
    }
    case ArmImport_subghz_protocol_blocks_add_bytes: {
        const uint8_t* message = arm_fap_vm_pointer(vm, p, q, false);
        if(!message) return false;
        result = subghz_protocol_blocks_add_bytes(message, q);
        break;
    }
    case ArmImport_subghz_protocol_blocks_add_to_128_bit: {
        SubGhzBlockDecoder* decoder = arm_fap_vm_pointer(vm, p, sizeof(*decoder), true);
        uint64_t* head = arm_fap_vm_pointer(vm, r, sizeof(*head), true);
        if(!decoder || !head) return false;
        subghz_protocol_blocks_add_to_128_bit(decoder, (uint8_t)q, head);
        break;
    }
    case ArmImport_subghz_protocol_blocks_crc4:
    case ArmImport_subghz_protocol_blocks_crc8: {
        const uint8_t* message = arm_fap_vm_pointer(vm, p, q, false);
        if(!message) return false;
        if(id == ArmImport_subghz_protocol_blocks_crc4)
            result = subghz_protocol_blocks_crc4(message, q, (uint8_t)r, (uint8_t)s);
        else
            result = subghz_protocol_blocks_crc8(message, q, (uint8_t)r, (uint8_t)s);
        break;
    }
    case ArmImport_subghz_protocol_blocks_get_hash_data: {
        SubGhzBlockDecoder* decoder = arm_fap_vm_pointer(vm, p, sizeof(*decoder), false);
        if(!decoder) return false;
        result = subghz_protocol_blocks_get_hash_data(decoder, q);
        break;
    }
    case ArmImport_subghz_protocol_blocks_lfsr_digest8:
    case ArmImport_subghz_protocol_blocks_lfsr_digest8_reflect: {
        const uint8_t* message = arm_fap_vm_pointer(vm, p, q, false);
        if(!message) return false;
        if(id == ArmImport_subghz_protocol_blocks_lfsr_digest8)
            result = subghz_protocol_blocks_lfsr_digest8(message, q, (uint8_t)r, (uint8_t)s);
        else
            result = subghz_protocol_blocks_lfsr_digest8_reflect(message, q, (uint8_t)r, (uint8_t)s);
        break;
    }
    case ArmImport_subghz_protocol_blocks_parity_bytes: {
        const uint8_t* message = arm_fap_vm_pointer(vm, p, q, false);
        if(!message) return false;
        result = subghz_protocol_blocks_parity_bytes(message, q);
        break;
    }
    case ArmImport_subghz_protocol_blocks_reverse_key: {
        uint64_t key = (uint64_t)p | ((uint64_t)q << 32);
        uint64_t reversed = subghz_protocol_blocks_reverse_key(key, (uint8_t)r);
        result = (uint32_t)reversed;
        vm->r[1] = (uint32_t)(reversed >> 32);
        break;
    }
    case ArmImport_subghz_receiver_alloc_init: {
        ArmSubGhzEnv* env = get_subghz_env(a, p); if(!env) return false;
        result = wrap_subghz_receiver(a, subghz_receiver_alloc_init(env->native));
        if(!result) return false;
        break;
    }
    case ArmImport_subghz_receiver_free:
        subghz_receiver_free(sg_receiver->native);
        memset(sg_receiver, 0, sizeof(*sg_receiver));
        break;
    case ArmImport_subghz_receiver_decode:
        subghz_receiver_decode(sg_receiver->native, q != 0, r);
        break;
    case ArmImport_subghz_receiver_reset: subghz_receiver_reset(sg_receiver->native); break;
    case ArmImport_subghz_receiver_set_filter:
        subghz_receiver_set_filter(sg_receiver->native, (SubGhzProtocolFlag)q);
        break;
    case ArmImport_subghz_setting_alloc:
        result = wrap_subghz_setting(a, subghz_setting_alloc());
        if(!result) return false;
        break;
    case ArmImport_subghz_setting_free:
        subghz_setting_free(sg_setting->native);
        memset(sg_setting, 0, sizeof(*sg_setting));
        break;
    case ArmImport_subghz_setting_get_default_frequency:
        result = subghz_setting_get_default_frequency(sg_setting->native); break;
    case ArmImport_subghz_setting_get_frequency:
        result = subghz_setting_get_frequency(sg_setting->native, q); break;
    case ArmImport_subghz_setting_get_frequency_count:
        result = (uint32_t)subghz_setting_get_frequency_count(sg_setting->native); break;
    case ArmImport_subghz_setting_get_frequency_default_index:
        result = subghz_setting_get_frequency_default_index(sg_setting->native); break;
    case ArmImport_subghz_setting_get_hopper_frequency:
        result = subghz_setting_get_hopper_frequency(sg_setting->native, q); break;
    case ArmImport_subghz_setting_get_hopper_frequency_count:
        result = (uint32_t)subghz_setting_get_hopper_frequency_count(sg_setting->native); break;
    case ArmImport_subghz_setting_get_inx_preset_by_name: {
        const char* name = arm_fap_vm_string(vm, q);
        if(!name) return false;
        size_t index = setting_preset_index(sg_setting->native, name);
        result = index == SIZE_MAX ? 0xffffffffu : (uint32_t)index;
        break;
    }
    case ArmImport_subghz_setting_get_preset_count:
        result = (uint32_t)subghz_setting_get_preset_count(sg_setting->native); break;
    case ArmImport_subghz_setting_get_preset_data:
    case ArmImport_subghz_setting_get_preset_data_by_name: {
        size_t index;
        if(id == ArmImport_subghz_setting_get_preset_data) {
            index = q;
        } else {
            const char* name = arm_fap_vm_string(vm, q);
            if(!name) return false;
            index = setting_preset_index(sg_setting->native, name);
        }
        if(index == SIZE_MAX || index >= subghz_setting_get_preset_count(sg_setting->native)) {
            result = 0; break;
        }
        uint8_t* data = subghz_setting_get_preset_data(sg_setting->native, index);
        size_t size = subghz_setting_get_preset_data_size(sg_setting->native, index);
        if(!data || !size || size > SUBGHZ_PRESET_BUF_SIZE) return false;
        if(!a->subghz_preset_buf) a->subghz_preset_buf = arm_fap_vm_alloc(vm, SUBGHZ_PRESET_BUF_SIZE);
        if(!a->subghz_preset_buf) return false;
        uint8_t* dest = arm_fap_vm_pointer(vm, a->subghz_preset_buf, size, true);
        if(!dest) return false;
        memcpy(dest, data, size);
        result = a->subghz_preset_buf;
        break;
    }
    case ArmImport_subghz_setting_get_preset_data_size:
        if(q >= subghz_setting_get_preset_count(sg_setting->native)) { result = 0; break; }
        result = (uint32_t)subghz_setting_get_preset_data_size(sg_setting->native, q);
        break;
    case ArmImport_subghz_setting_get_preset_name: {
        size_t count = subghz_setting_get_preset_count(sg_setting->native);
        if(!count || q >= count) { result = 0; break; }
        result = subghz_name_handle(a, subghz_setting_get_preset_name(sg_setting->native, q));
        break;
    }
    case ArmImport_subghz_setting_load: {
        const char* path = arm_fap_vm_string(vm, q);
        if(!path) return false;
        subghz_setting_load(sg_setting->native, path);
        break;
    }
    case ArmImport_subghz_setting_load_custom_preset: {
        const char* name = arm_fap_vm_string(vm, q);
        ArmFormat* format = get_format(a, r);
        if(!name || !format) return false;
        result = subghz_setting_load_custom_preset(sg_setting->native, name, format->native) ? 1 : 0;
        break;
    }
    case ArmImport_subghz_transmitter_alloc_init: {
        ArmSubGhzEnv* env = get_subghz_env(a, p); if(!env) return false;
        const char* name = arm_fap_vm_string(vm, q);
        if(!name) return false;
        result = wrap_subghz_transmitter(a, subghz_transmitter_alloc_init(env->native, name));
        if(!result) return false;
        break;
    }
    case ArmImport_subghz_transmitter_deserialize: {
        ArmFormat* format = get_format(a, q);
        if(!format) return false;
        result = (uint32_t)subghz_transmitter_deserialize(sg_transmitter->native, format->native);
        break;
    }
    case ArmImport_subghz_transmitter_free:
        subghz_transmitter_free(sg_transmitter->native);
        memset(sg_transmitter, 0, sizeof(*sg_transmitter));
        break;
    case ArmImport_subghz_transmitter_stop:
        result = subghz_transmitter_stop(sg_transmitter->native) ? 1 : 0; break;
    case ArmImport_subghz_transmitter_yield: {
        LevelDuration level_duration = subghz_transmitter_yield(sg_transmitter->native);
        memcpy(&result, &level_duration, sizeof(result));
        break;
    }
    case ArmImport_subghz_worker_alloc:
        for(unsigned i = 0; i < MAX_SUBGHZ_WORKERS; i++) if(!a->subghz_workers[i].native) {
            ArmSubGhzWorker* worker = &a->subghz_workers[i];
            worker->owner = a;
            worker->native = subghz_worker_alloc();
            subghz_worker_set_context(worker->native, worker);
            result = SUBGHZ_WORKER_HANDLE + i * 4; break;
        }
        if(!result) return false;
        break;
    case ArmImport_subghz_worker_free:
        if(subghz_worker_is_running(sg_worker->native)) subghz_worker_stop(sg_worker->native);
        subghz_worker_free(sg_worker->native);
        memset(sg_worker, 0, sizeof(*sg_worker));
        break;
    case ArmImport_subghz_worker_is_running:
        result = subghz_worker_is_running(sg_worker->native) ? 1 : 0; break;
    case ArmImport_subghz_worker_set_context: sg_worker->context = q; break;
    case ArmImport_subghz_worker_set_overrun_callback:
        sg_worker->overrun_callback = q;
        subghz_worker_set_overrun_callback(
            sg_worker->native, q ? subghz_worker_overrun_trampoline : NULL);
        break;
    case ArmImport_subghz_worker_set_pair_callback:
        sg_worker->pair_callback = q;
        subghz_worker_set_pair_callback(
            sg_worker->native, q ? subghz_worker_pair_trampoline : NULL);
        break;
    case ArmImport_subghz_worker_start: subghz_worker_start(sg_worker->native); break;
    case ArmImport_subghz_worker_stop: subghz_worker_stop(sg_worker->native); break;
    default: return false;
    }
    vm->r[0] = result;
    return !vm->error[0];
}

#define ARM_FAP_MISSING_LOG_DIR  EXT_PATH("apps_data/arm_fap")
#define ARM_FAP_MISSING_LOG_PATH EXT_PATH("apps_data/arm_fap/missing_apis.csv")
/* One row per failed load: app name, then every unresolved import for that
 * attempt (semicolon-joined, see arm_fap_vm.c's missing_imports). Appends
 * across runs/reboots so a user can just keep trying apps and collect a
 * cumulative report instead of reading one error off the small screen at a
 * time. CSV-quoted defensively; app/symbol names are not expected to contain
 * quotes, but a comma or semicolon in an app name is plausible. */
static void arm_fap_log_missing_imports(
    Storage* storage, const char* app_name, const char* missing) {
    if(!missing[0]) return;
    bool is_new = !storage_common_exists(storage, ARM_FAP_MISSING_LOG_PATH);
    storage_common_mkdir(storage, ARM_FAP_MISSING_LOG_DIR);
    File* file = storage_file_alloc(storage);
    if(storage_file_open(file, ARM_FAP_MISSING_LOG_PATH, FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FuriString* line = furi_string_alloc();
        if(is_new) furi_string_cat_str(line, "app_name,missing_apis\n");
        furi_string_cat_printf(line, "\"%s\",\"%s\"\n", app_name, missing);
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));
        furi_string_free(line);
    }
    storage_file_close(file);
    storage_file_free(file);
}
FlipperApplicationPreloadStatus arm_fap_runtime_preload(
    Storage* storage, const char* path, FlipperApplicationManifest* manifest,
    bool full, ArmFapRuntime** runtime) {
    File* file = storage_file_alloc(storage);
    uint8_t* data = NULL;
    FlipperApplicationPreloadStatus status = FlipperApplicationPreloadStatusInvalidFile;
    if(storage_file_open(file, path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        uint64_t size = storage_file_size(file);
        if(size >= 52 && size <= ARM_FAP_MAX_FILE) {
            data = heap_caps_malloc((size_t)size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if(!data) status = FlipperApplicationPreloadStatusNotEnoughMemory;
            else {
                size_t offset = 0;
                while(offset < size) {
                    size_t n = storage_file_read(file, data + offset, MIN(size - offset, 4096));
                    if(!n) break;
                    offset += n;
                }
                _Static_assert(sizeof(*manifest) == 85, "Unexpected native FAP manifest layout");
                if(offset == size && arm_fap_inspect(data, size, (uint8_t*)manifest)) {
                    manifest->name[sizeof(manifest->name) - 1] = 0;
                    status = FlipperApplicationPreloadStatusSuccess;
                    /* Native callers invoke plugin descriptors and callbacks
                     * directly. ARM guest pointers cannot be used by Xtensa. */
                    if(full && manifest->stack_size == 0) {
                        status = FlipperApplicationPreloadStatusTargetMismatch;
                        goto cleanup;
                    }
                    if(full) {
                        ArmFapRuntime* a = heap_caps_calloc(1, sizeof(*a), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                        if(a) {
                            a->vm.ram = heap_caps_calloc(1, ARM_FAP_RAM_SIZE, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                            if(a->vm.ram) {
                                a->mutex = furi_mutex_alloc(FuriMutexTypeRecursive);
                                a->vm.import = import_call; a->vm.context = a;
                                arm_fap_vm_load(&a->vm, data, size);
                                if(a->vm.missing_imports[0])
                                    arm_fap_log_missing_imports(
                                        storage, manifest->name, a->vm.missing_imports);
                                if(!a->vm.error[0]) a->event_buffer = arm_fap_vm_alloc(&a->vm, 8);
                                *runtime = a;
                            } else {
                                free(a);
                                status = FlipperApplicationPreloadStatusNotEnoughMemory;
                            }
                        } else status = FlipperApplicationPreloadStatusNotEnoughMemory;
                    }
                }
            }
        }
    }
cleanup:
    storage_file_free(file);
    free(data);
    return status;
}
const char* arm_fap_runtime_error(const ArmFapRuntime* a) {
    return a->vm.error[0] ? a->vm.error : NULL;
}

FlipperApplicationLoadStatus arm_fap_runtime_map(ArmFapRuntime* a) {
    if(a->vm.error[0]) {
        FURI_LOG_E(TAG, "%s", a->vm.error);
        return strstr(a->vm.error, "import:") ? FlipperApplicationLoadStatusMissingImports :
                                               FlipperApplicationLoadStatusUnspecifiedError;
    }
    FURI_LOG_I(TAG, "ARM/Thumb interpreter: arena=%u B in PSRAM, entry=0x%08lx", ARM_FAP_RAM_SIZE, (unsigned long)a->vm.entry);
    return FlipperApplicationLoadStatusSuccess;
}
static void cleanup_gui(ArmFapRuntime* a) {
    /* Detaching each view takes the GUI lock and waits out any in-flight draw. */
    for(unsigned i = 0; i < MAX_PORTS; i++) {
        ArmPort* port = &a->ports[i];
        if(port->added) { gui_remove_view_port(a->gui, port->native); port->added = false; }
        if(port->native) { view_port_free(port->native); port->native = NULL; }
    }
    for(unsigned i = 0; i < MAX_VIEWS; i++) {
        ArmView* v = &a->views[i];
        if(v->added) { view_dispatcher_remove_view(a->dispatcher, v->id); v->added = false; }
        if(v->native) { if(!v->external) view_free(v->native); v->native = NULL; }
    }
    if(a->dispatcher) { view_dispatcher_free(a->dispatcher); a->dispatcher = NULL; }
    if(a->holder.native) { view_holder_free(a->holder.native); memset(&a->holder, 0, sizeof(a->holder)); }
    for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(a->popups[i].native) {
        ArmPopup* module = &a->popups[i];
        popup_free(module->native); free(module->header); free(module->text);
        memset(module, 0, sizeof(*module));
    }
    for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(a->loadings[i].native) {
        ArmLoading* module = &a->loadings[i];
        loading_free(module->native);
        memset(module, 0, sizeof(*module));
    }
    for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(a->number_inputs[i].native) {
        ArmNumberInput* module = &a->number_inputs[i];
        number_input_free(module->native); free(module->header);
        memset(module, 0, sizeof(*module));
    }
    for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(a->byte_inputs[i].native) {
        ArmByteInput* module = &a->byte_inputs[i];
        byte_input_free(module->native); free(module->header);
        memset(module, 0, sizeof(*module));
    }
    for(unsigned i = 0; i < MAX_OTHER_MODULES; i++) if(a->menus[i].native) {
        ArmMenu* module = &a->menus[i];
        menu_free(module->native); for(unsigned j = 0; j < module->count; j++) free(module->entries[j].label);
        memset(module, 0, sizeof(*module));
    }
    for(unsigned i = 0; i < MAX_SUBMENUS; i++) if(a->submenus[i].native) {
        submenu_free(a->submenus[i].native); memset(&a->submenus[i], 0, sizeof(a->submenus[i]));
    }
    for(unsigned i = 0; i < MAX_TEXTBOXES; i++) if(a->text_boxes[i].native) {
        text_box_free(a->text_boxes[i].native); memset(&a->text_boxes[i], 0, sizeof(a->text_boxes[i]));
    }
    for(unsigned i = 0; i < MAX_VARLISTS; i++) if(a->var_lists[i].native) {
        variable_item_list_free(a->var_lists[i].native); memset(&a->var_lists[i], 0, sizeof(a->var_lists[i]));
    }
    memset(a->items, 0, sizeof(a->items));
    for(unsigned i = 0; i < MAX_WIDGETS; i++) if(a->widgets[i].native) {
        widget_free(a->widgets[i].native); memset(&a->widgets[i], 0, sizeof(a->widgets[i]));
    }
    for(unsigned i = 0; i < MAX_TEXT_INPUTS; i++) if(a->text_inputs[i].native) {
        text_input_free(a->text_inputs[i].native); memset(&a->text_inputs[i], 0, sizeof(a->text_inputs[i]));
    }
    for(unsigned i = 0; i < MAX_DIALOG_MESSAGES; i++) if(a->dialog_messages[i].native) {
        dialog_message_free(a->dialog_messages[i].native); a->dialog_messages[i].native = NULL;
    }
    for(unsigned i = 0; i < MAX_DIALOG_EX; i++) if(a->dialog_exs[i].native) {
        dialog_ex_free(a->dialog_exs[i].native); memset(&a->dialog_exs[i], 0, sizeof(a->dialog_exs[i]));
    }
    if(a->scene_managers[0].native) {
        scene_manager_free(a->scene_managers[0].native);
        memset(&a->scene_managers[0], 0, sizeof(a->scene_managers[0]));
    }
    if(a->infrared.native) {
        if(a->infrared.rx_running) infrared_worker_rx_stop(a->infrared.native);
        infrared_worker_free(a->infrared.native);
        memset(&a->infrared, 0, sizeof(a->infrared));
    }
    for(unsigned i = 0; i < MAX_NFC_PTRS; i++) {
        ArmNfcPtr* ptr = &a->nfc_ptrs[i];
        if(!ptr->native || !ptr->owned) continue;
        switch((NfcPtrKind)ptr->kind) {
        case NfcPtrNfc: nfc_free((Nfc*)ptr->native); break;
        case NfcPtrDevice: nfc_device_free((NfcDevice*)ptr->native); break;
        case NfcPtrPoller: nfc_poller_free((NfcPoller*)ptr->native); break;
        case NfcPtrScanner: nfc_scanner_free((NfcScanner*)ptr->native); break;
        case NfcPtrData: mf_classic_free((MfClassicData*)ptr->native); break;
        }
    }
    memset(a->nfc_ptrs, 0, sizeof(a->nfc_ptrs));
    while(a->dialogs_refs) { furi_record_close(RECORD_DIALOGS); a->dialogs_refs--; }
    if(a->direct_draw) { gui_direct_draw_release(a->gui); a->direct_draw = false; a->canvas = NULL; }
    for(unsigned i = 0; i < MAX_QUEUES; i++) if(a->queues[i].native) {
        furi_message_queue_free(a->queues[i].native); a->queues[i].native = NULL;
    }
    for(unsigned i = 0; i < MAX_STRINGS; i++) if(a->strings[i].native) {
        furi_string_free(a->strings[i].native); a->strings[i].native = NULL;
    }
    for(unsigned i = 0; i < MAX_FORMATS; i++) if(a->formats[i].native) {
        flipper_format_free(a->formats[i].native);
        memset(&a->formats[i], 0, sizeof(a->formats[i]));
    }
    for(unsigned i = 0; i < MAX_STREAMS; i++) if(a->streams[i].native && a->streams[i].owned) {
        stream_free(a->streams[i].native);
    }
    memset(a->streams, 0, sizeof(a->streams));
    for(unsigned i = 0; i < MAX_FILES; i++) if(a->files[i].native) {
        storage_file_free(a->files[i].native); a->files[i].native = NULL;
    }
    for(unsigned i = 0; i < MAX_BIT_BUFFERS; i++) if(a->bit_buffers[i].native) {
        bit_buffer_free(a->bit_buffers[i].native); memset(&a->bit_buffers[i], 0, sizeof(a->bit_buffers[i]));
    }
    for(unsigned i = 0; i < MAX_DIR_WALKS; i++) if(a->dir_walks[i].native) {
        dir_walk_free(a->dir_walks[i].native); memset(&a->dir_walks[i], 0, sizeof(a->dir_walks[i]));
    }
    while(a->storage_refs) { furi_record_close(RECORD_STORAGE); a->storage_refs--; }
    while(a->power_insomnia) { furi_hal_power_insomnia_exit(); a->power_insomnia--; }
    for(unsigned i = 0; i < MAX_MUTEXES; i++) if(a->mutexes[i].native) {
        furi_mutex_free(a->mutexes[i].native); a->mutexes[i].native = NULL;
    }
    for(unsigned i = 0; i < MAX_SEMAPHORES; i++) if(a->semaphores[i].native) {
        furi_semaphore_free(a->semaphores[i].native); a->semaphores[i].native = NULL;
    }
    for(unsigned i = 0; i < MAX_EVENT_FLAGS; i++) if(a->event_flags[i].native) {
        furi_event_flag_free(a->event_flags[i].native); a->event_flags[i].native = NULL;
    }
    for(unsigned i = 0; i < MAX_SUBGHZ_WORKERS; i++) if(a->subghz_workers[i].native) {
        if(subghz_worker_is_running(a->subghz_workers[i].native))
            subghz_worker_stop(a->subghz_workers[i].native);
        subghz_worker_free(a->subghz_workers[i].native);
    }
    memset(a->subghz_workers, 0, sizeof(a->subghz_workers));
    for(unsigned i = 0; i < MAX_SUBGHZ_RECEIVERS; i++) if(a->subghz_receivers[i].native) {
        subghz_receiver_free(a->subghz_receivers[i].native);
    }
    memset(a->subghz_receivers, 0, sizeof(a->subghz_receivers));
    for(unsigned i = 0; i < MAX_SUBGHZ_TRANSMITTERS; i++) if(a->subghz_transmitters[i].native) {
        subghz_transmitter_free(a->subghz_transmitters[i].native);
    }
    memset(a->subghz_transmitters, 0, sizeof(a->subghz_transmitters));
    for(unsigned i = 0; i < MAX_SUBGHZ_SETTINGS; i++) if(a->subghz_settings[i].native) {
        subghz_setting_free(a->subghz_settings[i].native);
    }
    memset(a->subghz_settings, 0, sizeof(a->subghz_settings));
    for(unsigned i = 0; i < MAX_SUBGHZ_ENVS; i++) if(a->subghz_envs[i].native) {
        subghz_environment_free(a->subghz_envs[i].native);
    }
    memset(a->subghz_envs, 0, sizeof(a->subghz_envs));
    for(unsigned i = 0; i < MAX_SUBGHZ_DEVICES; i++) if(a->subghz_devices[i].native) {
        subghz_devices_end(a->subghz_devices[i].native);
    }
    memset(a->subghz_devices, 0, sizeof(a->subghz_devices));
    if(a->subghz_devices_owned) { subghz_devices_deinit(); a->subghz_devices_owned = false; }
    if(a->backlight_forced && a->notification) {
        notification_message_block(a->notification, &sequence_display_backlight_enforce_auto);
        a->backlight_forced = false;
    }
    while(a->notification_refs) { furi_record_close(RECORD_NOTIFICATION); a->notification_refs--; }
    while(a->gui_refs) { furi_record_close(RECORD_GUI); a->gui_refs--; }
}
int32_t arm_fap_runtime_run(ArmFapRuntime* a) {
    uint32_t argument = 0, result = 0;
    lock(a);
    bool ok = guest_call(a, a->vm.entry, &argument, 1, &result);
    unlock(a);
    cleanup_gui(a);
    FURI_LOG_I(TAG, "ARM app finished: instructions=%llu, result=%ld", (unsigned long long)a->vm.instructions, (long)(ok ? result : -1));
    if(!ok) {
        FURI_LOG_E(TAG, "%s at 0x%08lx opcode=%08lx", a->vm.error, (unsigned long)a->vm.fault_pc, (unsigned long)a->vm.fault_instruction);
        DialogsApp* dialogs = furi_record_open(RECORD_DIALOGS);
        DialogMessage* message = dialog_message_alloc();
        dialog_message_set_header(message, "ARM app stopped", 64, 3, AlignCenter, AlignTop);
        dialog_message_set_text(message, a->vm.error, 0, 22, AlignLeft, AlignTop);
        dialog_message_set_buttons(message, "Back", NULL, NULL);
        dialog_message_show(dialogs, message);
        dialog_message_free(message);
        furi_record_close(RECORD_DIALOGS);
    }
    return ok ? (int32_t)result : -1;
}
void arm_fap_runtime_free(ArmFapRuntime* a) {
    if(!a) return;
    cleanup_gui(a);
    if(a->compress) compress_free(a->compress);
    if(a->mutex) furi_mutex_free(a->mutex);
    free(a->vm.ram);
    free(a);
}
