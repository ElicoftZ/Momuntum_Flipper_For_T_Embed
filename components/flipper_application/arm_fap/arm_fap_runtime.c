#include "arm_fap_runtime.h"
#include "arm_fap_vm.h"
#include <esp_heap_caps.h>
#include <furi_hal_random.h>
#include <gui/gui.h>
#include <gui/view.h>
#include <gui/view_dispatcher.h>
#include <dialogs/dialogs.h>
#include <toolbox/compress.h>
#include <notification/notification_messages.h>

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

typedef struct {
    ArmFapRuntime* owner;
    View* native;
    uint32_t model, context, draw, input, id;
    bool added;
} ArmView;

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

struct ArmFapRuntime {
    ArmFapVm vm;
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
static bool import_call(ArmFapVm* vm, ArmFapImport id, void* context) {
    ArmFapRuntime* a = context;
    uint32_t p = vm->r[0], q = vm->r[1], r = vm->r[2], s = vm->r[3], result = 0;
    ArmView* v = NULL;
    ArmPort* port = NULL;
    if(id >= ArmImport_view_free && id <= ArmImport_view_set_input_callback) {
        v = get_view(a, p); if(!v) return false;
    }
    if(id >= ArmImport_view_dispatcher_free && id <= ArmImport_view_dispatcher_stop) {
        if(!has_dispatcher(a, p)) return false;
    }
    if((id >= ArmImport_canvas_clear && id <= ArmImport_canvas_draw_str_aligned) ||
       (id >= ArmImport_canvas_draw_box && id <= ArmImport_canvas_draw_str)) {
        if(p != CANVAS_HANDLE || !a->canvas) { fault(a, "Canvas used outside draw callback"); return false; }
        if(id >= ArmImport_canvas_draw_icon &&
           ((int32_t)q < -1024 || (int32_t)q > 1024 || (int32_t)r < -1024 || (int32_t)r > 1024)) return false;
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
        if(v->added) return false;
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
    default: return false;
    }
    vm->r[0] = result;
    return !vm->error[0];
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
        if(v->native) { view_free(v->native); v->native = NULL; }
    }
    if(a->dispatcher) { view_dispatcher_free(a->dispatcher); a->dispatcher = NULL; }
    for(unsigned i = 0; i < MAX_QUEUES; i++) if(a->queues[i].native) {
        furi_message_queue_free(a->queues[i].native); a->queues[i].native = NULL;
    }
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
