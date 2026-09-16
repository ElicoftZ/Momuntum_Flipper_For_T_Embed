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
    X(_ctype_) \
    X(abort) X(__assert_func) X(__errno) X(calloc) X(realloc) X(memchr) X(memcmp) X(strcasecmp) \
    X(strchr) X(strcpy) X(strdup) X(strncasecmp) X(strncmp) X(strncpy) X(strrchr) X(strstr) \
    X(strtof) X(strtol) X(strtoul) X(strtoull) X(atoi) X(random) X(srand) X(roundf) \
    X(strint_to_uint32) X(value_index_uint32) X(hex_char_to_uint8) X(bit_lib_bytes_to_num_bcd) \
    X(bit_lib_bytes_to_num_be) X(bit_lib_bytes_to_num_le) X(bit_lib_get_bits) X(bit_lib_get_bits_16) \
    X(bit_lib_get_bits_32) X(bit_lib_get_bits_64) X(bit_lib_num_to_bytes_be) \
    X(popup_alloc) X(popup_free) X(popup_get_view) X(popup_reset) X(popup_set_callback) \
    X(popup_set_context) X(popup_set_header) X(popup_set_text) X(popup_set_timeout) \
    X(popup_enable_timeout) X(popup_disable_timeout) \
    X(loading_alloc) X(loading_free) X(loading_get_view) \
    X(number_input_alloc) X(number_input_free) X(number_input_get_view) \
    X(number_input_set_header_text) X(number_input_set_result_callback) \
    X(byte_input_alloc) X(byte_input_free) X(byte_input_get_view) X(byte_input_set_header_text) \
    X(byte_input_set_result_callback) \
    X(menu_alloc) X(menu_free) X(menu_get_view) X(menu_reset) X(menu_add_item) \
    X(bit_buffer_alloc) X(bit_buffer_free) X(bit_buffer_reset) X(bit_buffer_append_byte) \
    X(bit_buffer_append_bytes) X(bit_buffer_get_data) X(bit_buffer_get_size_bytes) X(dir_walk_alloc) \
    X(dir_walk_free) X(dir_walk_open) X(dir_walk_read) X(dir_walk_set_recursive) \
    X(file_stream_alloc) X(file_stream_open) X(file_stream_close) X(buffered_file_stream_alloc) \
    X(buffered_file_stream_open) X(buffered_file_stream_close) \
    X(args_read_int_and_trim) X(args_read_probably_quoted_string_and_trim) \
    X(args_read_string_and_trim) X(datetime_datetime_to_timestamp) X(datetime_get_days_per_month) \
    X(datetime_get_days_per_year) X(datetime_is_leap_year) X(datetime_timestamp_to_datetime) \
    X(locale_celsius_to_fahrenheit) X(locale_fahrenheit_to_celsius) X(locale_format_date) \
    X(locale_format_time) X(locale_get_date_format) X(locale_get_time_format) \
    X(path_extract_extension) X(path_extract_filename) X(path_extract_filename_no_ext) \
    X(saved_struct_load) X(saved_struct_save) X(pretty_format_bytes_hex_canonical) \
    X(manchester_advance) X(memmgr_get_free_heap) X(dolphin_deed) \
    X(malloc) X(free) X(__furi_crash_implementation) X(furi_hal_random_get) \
    X(furi_record_open) X(furi_record_close) \
    X(view_alloc) X(view_free) X(view_allocate_model) X(view_get_model) \
    X(view_commit_model) X(view_set_context) X(view_set_draw_callback) \
    X(view_set_input_callback) X(view_free_model) X(view_set_enter_callback) \
    X(view_set_exit_callback) X(view_set_previous_callback) X(view_set_orientation) \
    X(view_dispatcher_alloc) X(view_dispatcher_free) \
    X(view_dispatcher_set_event_callback_context) \
    X(view_dispatcher_set_navigation_event_callback) \
    X(view_dispatcher_set_tick_event_callback) X(view_dispatcher_add_view) \
    X(view_dispatcher_remove_view) X(view_dispatcher_attach_to_gui) \
    X(view_dispatcher_switch_to_view) X(view_dispatcher_run) X(view_dispatcher_stop) \
    X(view_dispatcher_enable_queue) X(view_dispatcher_send_custom_event) \
    X(view_dispatcher_set_custom_event_callback) \
    X(view_holder_alloc) X(view_holder_free) X(view_holder_set_view) \
    X(view_holder_set_back_callback) X(view_holder_attach_to_gui) \
    X(gui_direct_draw_acquire) X(gui_direct_draw_release) X(view_port_set_orientation) \
    X(submenu_alloc) X(submenu_free) X(submenu_get_view) X(submenu_add_item) \
    X(submenu_change_item_label) X(submenu_reset) X(submenu_set_header) \
    X(submenu_set_selected_item) \
    X(text_box_alloc) X(text_box_free) X(text_box_get_view) X(text_box_reset) \
    X(text_box_set_text) X(text_box_set_font) X(text_box_set_focus) \
    X(variable_item_list_alloc) X(variable_item_list_free) X(variable_item_list_get_view) \
    X(variable_item_list_reset) X(variable_item_list_set_selected_item) \
    X(variable_item_list_set_enter_callback) X(variable_item_list_add) \
    X(variable_item_get_context) X(variable_item_get_current_value_index) \
    X(variable_item_set_current_value_index) X(variable_item_set_current_value_text) \
    X(widget_alloc) X(widget_free) X(widget_get_view) X(widget_reset) \
    X(widget_add_string_element) X(widget_add_string_multiline_element) \
    X(widget_add_text_box_element) X(widget_add_text_scroll_element) \
    X(widget_add_button_element) X(widget_add_rect_element) \
    X(text_input_alloc) X(text_input_free) X(text_input_get_view) X(text_input_reset) \
    X(text_input_set_result_callback) X(text_input_set_minimum_length) \
    X(text_input_set_header_text) \
    X(dialog_message_alloc) X(dialog_message_free) X(dialog_message_set_header) \
    X(dialog_message_set_text) X(dialog_message_set_buttons) X(dialog_message_show) \
    X(dialog_message_show_storage_error) \
    X(dialog_ex_alloc) X(dialog_ex_free) X(dialog_ex_get_view) X(dialog_ex_reset) \
    X(dialog_ex_set_context) X(dialog_ex_set_result_callback) X(dialog_ex_set_header) \
    X(dialog_ex_set_text) X(dialog_ex_set_left_button_text) \
    X(dialog_ex_set_center_button_text) X(dialog_ex_set_right_button_text) \
    X(scene_manager_alloc) X(scene_manager_free) X(scene_manager_get_scene_state) \
    X(scene_manager_handle_back_event) X(scene_manager_handle_custom_event) \
    X(scene_manager_handle_tick_event) X(scene_manager_has_previous_scene) \
    X(scene_manager_next_scene) X(scene_manager_previous_scene) \
    X(scene_manager_search_and_switch_to_another_scene) \
    X(scene_manager_search_and_switch_to_previous_scene) \
    X(scene_manager_search_and_switch_to_previous_scene_one_of) \
    X(scene_manager_set_scene_state) X(scene_manager_stop) \
    X(infrared_get_protocol_address_length) X(infrared_get_protocol_by_name) \
    X(infrared_get_protocol_command_length) X(infrared_get_protocol_name) \
    X(infrared_is_protocol_valid) X(infrared_send) X(infrared_send_raw_ext) \
    X(infrared_worker_alloc) X(infrared_worker_free) X(infrared_worker_get_decoded_signal) \
    X(infrared_worker_get_raw_signal) X(infrared_worker_rx_set_received_signal_callback) \
    X(infrared_worker_rx_start) X(infrared_worker_rx_stop) \
    X(infrared_worker_signal_is_decoded) \
    X(nfc_alloc) X(nfc_free) \
    X(nfc_device_alloc) X(nfc_device_free) X(nfc_device_get_data) X(nfc_device_set_data) \
    X(nfc_device_copy_data) X(nfc_device_get_name) X(nfc_device_get_protocol) \
    X(nfc_device_set_loading_callback) \
    X(nfc_poller_alloc) X(nfc_poller_free) X(nfc_poller_get_data) X(nfc_poller_stop) \
    X(nfc_scanner_alloc) X(nfc_scanner_free) X(nfc_scanner_start) X(nfc_scanner_stop) \
    X(mf_classic_alloc) X(mf_classic_free) X(mf_classic_block_to_value) \
    X(mf_classic_get_first_block_num_of_sector) X(mf_classic_get_sector_by_block) \
    X(mf_classic_get_sector_trailer_by_sector) X(mf_classic_get_sector_trailer_num_by_block) \
    X(mf_classic_get_total_sectors_num) X(mf_classic_get_uid) X(mf_classic_is_block_read) \
    X(mf_classic_is_card_read) \
    X(mf_classic_poller_sync_auth) X(mf_classic_poller_sync_detect_type) \
    X(mf_classic_poller_sync_read) X(mf_classic_poller_sync_read_block) \
    X(mf_ultralight_get_pages_total) \
    X(iso15693_3_get_block_count) X(iso15693_3_get_block_size) \
    X(canvas_clear) X(canvas_set_color) X(canvas_set_font) \
    X(canvas_width) X(canvas_height) X(canvas_current_font_height) X(canvas_string_width) \
    X(canvas_invert_color) X(elements_button_left) X(elements_button_center) \
    X(elements_button_right) X(canvas_draw_icon) \
    X(canvas_draw_rframe) X(canvas_draw_disc) X(canvas_draw_str_aligned) \
    X(canvas_draw_dot) X(canvas_draw_circle) X(elements_multiline_text) \
    X(elements_scrollbar_pos) X(elements_text_box) \
    X(memcpy) X(memmove) X(memset) X(strlen) X(strcmp) X(strlcpy) X(strlcat) \
    X(furi_delay_ms) X(furi_get_tick) X(furi_ms_to_ticks) X(rand) \
    X(canvas_draw_box) X(canvas_draw_frame) X(canvas_draw_line) X(canvas_draw_str) \
    X(furi_message_queue_alloc) X(furi_message_queue_free) X(furi_message_queue_get) \
    X(furi_message_queue_put) X(furi_message_queue_reset) \
    X(view_port_alloc) X(view_port_free) X(view_port_draw_callback_set) \
    X(view_port_input_callback_set) X(view_port_update) X(view_port_enabled_set) \
    X(gui_add_view_port) X(gui_remove_view_port) \
    X(notification_message) X(notification_message_block) \
    X(sequence_display_backlight_enforce_on) X(sequence_display_backlight_enforce_auto) \
    X(furi_string_alloc) X(furi_string_alloc_set_str) X(furi_string_alloc_set) \
    X(furi_string_free) \
    X(furi_string_reset) X(furi_string_set_str) X(furi_string_set_strn) \
    X(furi_string_cat_str) X(furi_string_push_back) X(furi_string_set_char) \
    X(furi_string_get_char) X(furi_string_get_cstr) X(furi_string_size) \
    X(furi_string_empty) X(furi_string_equal_str) X(furi_string_cmp_str) \
    X(furi_string_start_with_str) X(furi_string_reserve) X(furi_string_move) \
    X(furi_string_set) X(furi_string_set_n) X(furi_string_cat) X(furi_string_cmp) \
    X(furi_string_cmpi_str) X(furi_string_search_str) X(furi_string_search_char) \
    X(furi_string_search_rchar) X(furi_string_equal) X(furi_string_replace_str) \
    X(furi_string_replace_all_str) X(furi_string_left) X(furi_string_right) \
    X(furi_string_trim) X(furi_string_utf8_length) \
    X(storage_file_alloc) \
    X(storage_file_free) X(storage_file_open) X(storage_file_close) \
    X(storage_file_read) X(storage_file_write) X(storage_file_seek) \
    X(storage_file_tell) X(storage_file_size) X(storage_file_get_error) \
    X(storage_file_get_error_desc) X(storage_dir_open) X(storage_dir_close) \
    X(storage_dir_read) \
    X(storage_file_exists) X(storage_dir_exists) X(storage_common_copy) \
    X(storage_common_exists) X(storage_common_migrate) X(storage_common_mkdir) \
    X(storage_common_remove) X(storage_common_rename) X(storage_common_stat) \
    X(storage_sd_status) X(storage_simply_mkdir) X(storage_simply_remove) \
    X(storage_simply_remove_recursive) \
    X(flipper_format_string_alloc) X(flipper_format_file_alloc) \
    X(flipper_format_buffered_file_alloc) \
    X(flipper_format_free) X(flipper_format_file_close) \
    X(flipper_format_file_open_always) X(flipper_format_file_open_append) \
    X(flipper_format_file_open_existing) X(flipper_format_file_open_new) \
    X(flipper_format_buffered_file_open_existing) \
    X(flipper_format_rewind) X(flipper_format_seek_to_end) \
    X(flipper_format_get_raw_stream) X(flipper_format_get_value_count) \
    X(flipper_format_read_bool) X(flipper_format_read_float) \
    X(flipper_format_read_hex) X(flipper_format_read_int32) \
    X(flipper_format_read_uint32) X(flipper_format_read_header) \
    X(flipper_format_read_string) \
    X(flipper_format_write_bool) X(flipper_format_write_float) \
    X(flipper_format_write_hex) X(flipper_format_write_uint32) \
    X(flipper_format_write_string) X(flipper_format_write_string_cstr) \
    X(flipper_format_write_header_cstr) X(flipper_format_write_comment_cstr) \
    X(flipper_format_update_hex) X(flipper_format_update_uint32) \
    X(flipper_format_insert_or_update_bool) X(flipper_format_insert_or_update_float) \
    X(flipper_format_insert_or_update_hex) \
    X(flipper_format_insert_or_update_string_cstr) \
    X(flipper_format_insert_or_update_uint32) \
    X(stream_clean) X(stream_copy) X(stream_delete) X(stream_eof) \
    X(stream_free) X(stream_insert) X(stream_read) X(stream_read_line) \
    X(stream_rewind) X(stream_seek) X(stream_seek_to_char) X(stream_size) \
    X(stream_tell) X(stream_write) X(stream_write_char) \
    X(furi_delay_tick) X(furi_kernel_get_tick_frequency) \
    X(furi_log_print_format) X(furi_log_set_level) \
    X(furi_mutex_alloc) X(furi_mutex_free) X(furi_mutex_acquire) X(furi_mutex_release) \
    X(furi_semaphore_alloc) X(furi_semaphore_free) X(furi_semaphore_acquire) \
    X(furi_semaphore_release) \
    X(furi_event_flag_alloc) X(furi_event_flag_free) X(furi_event_flag_set) \
    X(furi_event_flag_wait) \
    X(furi_hal_bt_extra_beacon_is_active) X(furi_hal_bt_extra_beacon_set_config) \
    X(furi_hal_bt_extra_beacon_set_data) X(furi_hal_bt_extra_beacon_start) \
    X(furi_hal_bt_extra_beacon_stop) X(furi_hal_bt_start_advertising) \
    X(furi_hal_bt_stop_advertising) \
    X(furi_hal_crypto_decrypt) X(furi_hal_crypto_enclave_ensure_key) \
    X(furi_hal_crypto_enclave_load_key) X(furi_hal_crypto_enclave_unload_key) \
    X(furi_hal_crypto_encrypt) \
    X(furi_hal_hid_is_connected) X(furi_hal_hid_kb_press) X(furi_hal_hid_kb_release) \
    X(furi_hal_hid_kb_release_all) \
    X(furi_hal_infrared_detect_tx_output) X(furi_hal_infrared_set_tx_output) \
    X(furi_hal_nfc_field_detect_start) X(furi_hal_nfc_field_detect_stop) \
    X(furi_hal_nfc_field_is_present) \
    X(furi_hal_power_disable_otg) X(furi_hal_power_enable_otg) \
    X(furi_hal_power_get_battery_full_capacity) \
    X(furi_hal_power_get_battery_remaining_capacity) X(furi_hal_power_insomnia_enter) \
    X(furi_hal_power_insomnia_exit) X(furi_hal_power_is_otg_enabled) \
    X(furi_hal_power_suppress_charge_enter) X(furi_hal_power_suppress_charge_exit) \
    X(furi_hal_random_fill_buf) \
    X(furi_hal_rfid_field_detect_start) X(furi_hal_rfid_field_detect_stop) \
    X(furi_hal_rfid_field_is_present) \
    X(furi_hal_rtc_get_datetime) X(furi_hal_rtc_get_locale_units) \
    X(furi_hal_rtc_get_timestamp) X(furi_hal_rtc_is_flag_set) \
    X(furi_hal_speaker_acquire) X(furi_hal_speaker_is_mine) X(furi_hal_speaker_release) \
    X(furi_hal_speaker_stop) \
    X(furi_hal_usb_unlock) X(furi_hal_version_uid) X(furi_hal_version_uid_size) \
    X(subghz_block_generic_deserialize) \
    X(subghz_block_generic_deserialize_check_count_bit) \
    X(subghz_devices_init) X(subghz_devices_deinit) X(subghz_devices_get_by_name) \
    X(subghz_devices_begin) X(subghz_devices_end) X(subghz_devices_flush_rx) \
    X(subghz_devices_get_rssi) X(subghz_devices_idle) X(subghz_devices_is_connect) \
    X(subghz_devices_is_frequency_valid) X(subghz_devices_load_preset) \
    X(subghz_devices_reset) X(subghz_devices_set_frequency) X(subghz_devices_set_rx) \
    X(subghz_devices_set_tx) X(subghz_devices_sleep) \
    X(subghz_environment_alloc) X(subghz_environment_free) \
    X(subghz_environment_get_protocol_name_registry) X(subghz_environment_load_keystore) \
    X(subghz_keystore_raw_get_data) \
    X(subghz_protocol_blocks_add_bit) X(subghz_protocol_blocks_add_bytes) \
    X(subghz_protocol_blocks_add_to_128_bit) X(subghz_protocol_blocks_crc4) \
    X(subghz_protocol_blocks_crc8) X(subghz_protocol_blocks_get_hash_data) \
    X(subghz_protocol_blocks_lfsr_digest8) X(subghz_protocol_blocks_lfsr_digest8_reflect) \
    X(subghz_protocol_blocks_parity_bytes) X(subghz_protocol_blocks_reverse_key) \
    X(subghz_receiver_alloc_init) X(subghz_receiver_free) X(subghz_receiver_decode) \
    X(subghz_receiver_reset) X(subghz_receiver_set_filter) \
    X(subghz_setting_alloc) X(subghz_setting_free) \
    X(subghz_setting_get_default_frequency) X(subghz_setting_get_frequency) \
    X(subghz_setting_get_frequency_count) X(subghz_setting_get_frequency_default_index) \
    X(subghz_setting_get_hopper_frequency) X(subghz_setting_get_hopper_frequency_count) \
    X(subghz_setting_get_inx_preset_by_name) X(subghz_setting_get_preset_count) \
    X(subghz_setting_get_preset_data) X(subghz_setting_get_preset_data_by_name) \
    X(subghz_setting_get_preset_data_size) X(subghz_setting_get_preset_name) \
    X(subghz_setting_load) X(subghz_setting_load_custom_preset) \
    X(subghz_transmitter_alloc_init) X(subghz_transmitter_deserialize) \
    X(subghz_transmitter_free) X(subghz_transmitter_stop) X(subghz_transmitter_yield) \
    X(subghz_worker_alloc) X(subghz_worker_free) X(subghz_worker_is_running) \
    X(subghz_worker_set_context) X(subghz_worker_set_overrun_callback) \
    X(subghz_worker_set_pair_callback) X(subghz_worker_start) X(subghz_worker_stop)

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
    /* Every unresolved import name hit during relocation, semicolon-joined,
     * not just the first -- lets a caller log the complete gap for one FAP
     * in a single load attempt instead of one symbol at a time. */
    char missing_imports[256];
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
