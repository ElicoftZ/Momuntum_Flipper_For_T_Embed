#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Guest addresses are offsets in a bounded arena, never native pointers. */
#define ARM_FAP_BASE 0x10000000u
#define ARM_FAP_RAM_SIZE (64u * 1024u)
#define ARM_FAP_STACK_SIZE 8192u
#define ARM_FAP_IMPORT_BASE 0x08000000u
#define ARM_FAP_RETURN 0x08001000u
#define ARM_FAP_MAX_FILE (256u * 1024u)
#define ARM_FAP_MAX_SECTIONS 64u

#define ARM_FAP_IMPORTS(X) \
    X(malloc) X(free) X(__furi_crash_implementation) X(furi_hal_random_get) \
    X(furi_record_open) X(furi_record_close) \
    X(view_alloc) X(view_free) X(view_allocate_model) X(view_get_model) \
    X(view_commit_model) X(view_set_context) X(view_set_draw_callback) \
    X(view_set_input_callback) X(view_dispatcher_alloc) X(view_dispatcher_free) \
    X(view_dispatcher_set_event_callback_context) \
    X(view_dispatcher_set_navigation_event_callback) \
    X(view_dispatcher_set_tick_event_callback) X(view_dispatcher_add_view) \
    X(view_dispatcher_remove_view) X(view_dispatcher_attach_to_gui) \
    X(view_dispatcher_switch_to_view) X(view_dispatcher_run) X(view_dispatcher_stop) \
    X(canvas_clear) X(canvas_set_color) X(canvas_set_font) X(canvas_draw_icon) \
    X(canvas_draw_rframe) X(canvas_draw_disc) X(canvas_draw_str_aligned) \
    X(memcpy) X(memmove) X(memset) X(strlen) X(strcmp) X(strlcpy) X(strlcat) \
    X(furi_delay_ms) X(furi_get_tick) X(furi_ms_to_ticks) X(rand) \
    X(canvas_draw_box) X(canvas_draw_frame) X(canvas_draw_line) X(canvas_draw_str) \
    X(furi_message_queue_alloc) X(furi_message_queue_free) X(furi_message_queue_get) \
    X(furi_message_queue_put) X(furi_message_queue_reset) \
    X(view_port_alloc) X(view_port_free) X(view_port_draw_callback_set) \
    X(view_port_input_callback_set) X(view_port_update) X(view_port_enabled_set) \
    X(gui_add_view_port) X(gui_remove_view_port) \
    X(notification_message) X(notification_message_block) \
    X(sequence_display_backlight_enforce_on) X(sequence_display_backlight_enforce_auto)

typedef enum {
#define ARM_FAP_ENUM(name) ArmImport_##name,
    ARM_FAP_IMPORTS(ARM_FAP_ENUM)
#undef ARM_FAP_ENUM
    ArmImportCount,
} ArmFapImport;

typedef struct ArmFapVm ArmFapVm;
typedef bool (*ArmFapImportCallback)(ArmFapVm*, ArmFapImport, void*);
typedef struct {
    uint32_t address, size, flags;
} ArmFapRegion;

struct ArmFapVm {
    uint8_t* ram;
    uint32_t r[16];
    bool n, z, c, v;
    uint8_t itstate;
    uint32_t entry, heap_start, heap_end, heap_limit;
    struct { uint32_t address, size; bool used; } allocations[32];
    ArmFapRegion regions[ARM_FAP_MAX_SECTIONS];
    size_t region_count;
    ArmFapImportCallback import;
    void* context;
    uint64_t instructions;
    uint32_t yields;
    uint32_t fault_pc, fault_instruction;
    char error[112];
};

/* Copy the 85-byte Flipper v1 manifest; a zero Momentum flags byte is allowed. */
bool arm_fap_inspect(const uint8_t* file, size_t size, uint8_t manifest[85]);
bool arm_fap_vm_load(ArmFapVm* vm, const uint8_t* file, size_t size);
bool arm_fap_vm_call(
    ArmFapVm* vm, uint32_t entry, const uint32_t* args, size_t argc,
    uint32_t budget, uint32_t* result);
void arm_fap_vm_fault(ArmFapVm* vm, const char* error);
void* arm_fap_vm_pointer(ArmFapVm* vm, uint32_t address, size_t size, bool write);
const char* arm_fap_vm_string(ArmFapVm* vm, uint32_t address);
uint32_t arm_fap_vm_read(ArmFapVm* vm, uint32_t address, unsigned bytes);
bool arm_fap_vm_write(ArmFapVm* vm, uint32_t address, uint32_t value, unsigned bytes);
uint32_t arm_fap_vm_alloc(ArmFapVm* vm, size_t size);
bool arm_fap_vm_free(ArmFapVm* vm, uint32_t address);
