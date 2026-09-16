# Other-library ARM bridges

This pass adds **106 imports** from the 207-symbol native-export candidate bucket.
The regenerated API inventory contains 565 bridged declarations in total; matching
declarations is not proof of full ABI or hardware compatibility. Existing uncommitted
bridge work was preserved.

## Implemented

- 24 libc/allocator imports, including guest-address pointer returns, guest-only abort/assert,
  bounded comparisons/copies, 64-bit integer returns in r0/r1, float bit marshalling,
  guest realloc/calloc and per-runtime errno scratch.
- 11 bit/conversion helpers. Bit extraction avoids the native helper’s unaligned-byte
  overread. Widths and byte ranges are checked before access.
- 29 GUI imports across Popup, Loading, NumberInput, ByteInput and Menu. Two instances
  per module share the existing four-view limit. Retained labels/headers have native
  copies; callbacks carry wrapper contexts. Menu has up to 24 entries, each with its
  own callback/context. Only null menu icons are accepted (native default icon).
- Seven BitBuffer imports, with four opaque handles, fixed capacities bounded by guest
  memory, append overflow checks and persistent guest snapshots.
- Eleven file-stream and directory-walk imports. File and buffered streams use the
  existing four-stream pool and track their concrete type. Two directory walkers are
  supported. FileInfo fields are marshalled to ARM offsets explicitly.
- 23 date, locale, path, argument, saved-structure, Manchester, heap-query and dolphin
  helpers. DateTime and short enums are marshalled explicitly.
- `_ctype_` is a data relocation, not a function trap: a 257-byte Newlib table is copied
  into a read-only guest region once per loaded image. The linked native symbol size
  was checked with the Xtensa toolchain.

## Ownership and limits

Module views reject generic guest model/callback/disposal operations. Cleanup frees
them through their owning module once. Freeing a module whose view remains added to
a dispatcher, or while a view holder exists, is rejected. Module operations from
module callbacks are rejected to avoid native GUI/model-lock re-entry; callbacks can
enqueue dispatcher custom events for subsequent updates. ByteInput maintains a native
working buffer and synchronizes guest bytes on change/result callbacks. Guest changes
made outside those callbacks require another set-result-callback call to resynchronize.

The arena remains 64 KiB with 32 allocator slots and a 1,024-byte C-string scan limit.
Pretty-format lines are bounded to 256 columns. Date input is checked before native
calendar operations. These remain bounded subsets, not unrestricted native semantics.

BitBuffer removes the buffer-wrapper prerequisite for a future pass on
`iso14443_4a_poller_send_block` and `iso14443_4b_poller_send_block`. Those NFC imports
were not added here; they still need their own bridge cases and validation.

## Deferred (101 imports)

### Bluetooth state and callbacks (10)

All candidates mutate state, send data, or install callbacks; none is a standalone read-only query. Native profile ownership/thread cleanup needs a separate decision.

`ble_profile_serial_notify_buffer_is_empty`, `ble_profile_serial_set_event_callback`, `ble_profile_serial_set_rpc_active`, `ble_profile_serial_tx`, `bt_disconnect`, `bt_keys_storage_set_default_path`, `bt_keys_storage_set_storage_path`, `bt_profile_restore_default`, `bt_profile_start`, `bt_set_status_changed_callback`.

### CLI and pipes (5)

No guest CliRegistry or PipeSide handle producer exists. CLI callbacks also run on another native thread and need registration cleanup. Accepting arbitrary pointers would violate the bridge invariant.

`cli_is_pipe_broken_or_is_etx_next_char`, `cli_registry_add_command`, `cli_registry_delete_command`, `pipe_receive`, `pipe_send`.

### Nested loader and plugin machinery (15)

Loading another guest/plugin recursively requires an explicit loader and trust design.

`composite_api_resolver_add`, `composite_api_resolver_alloc`, `composite_api_resolver_free`, `composite_api_resolver_get`, `elf_resolve_from_hashtable`, `flipper_application_alloc`, `flipper_application_free`, `flipper_application_is_plugin`, `flipper_application_map_to_memory`, `flipper_application_plugin_get_descriptor`, `flipper_application_preload`, `plugin_manager_alloc`, `plugin_manager_free`, `plugin_manager_get_ep`, `plugin_manager_load_single`.

### Compression codec (4)

Native compress_decode_internal trusts compressed_buff_size without checking the supplied input length. Its uncompressed branch copies data_in_size bytes from data_in + 1 after checking capacity against data_in_size - 1. Codec bounds/progress handling must be fixed and tested before exposing these objects to guests.

`compress_alloc`, `compress_decode`, `compress_encode`, `compress_free`.

### Persistent icons (8)

Persistent native icon/frame/bitmap ownership is not implemented.

`compress_icon_alloc`, `compress_icon_decode`, `compress_icon_free`, `icon_animation_alloc`, `icon_animation_free`, `icon_animation_start`, `icon_get_frame_data`, `popup_set_icon`.

### JavaScript engine (55)

Nested JS runtime, GC/value ownership and FFI are outside this pass.

`mjs_arg`, `mjs_array_buf_get_ptr`, `mjs_array_del`, `mjs_array_get`, `mjs_array_length`, `mjs_array_push`, `mjs_array_set`, `mjs_call`, `mjs_create`, `mjs_del`, `mjs_destroy`, `mjs_disown`, `mjs_exec`, `mjs_exec_file`, `mjs_exit`, `mjs_get`, `mjs_get_bool`, `mjs_get_context`, `mjs_get_double`, `mjs_get_global`, `mjs_get_int`, `mjs_get_ptr`, `mjs_get_stack_trace`, `mjs_get_string`, `mjs_is_array`, `mjs_is_boolean`, `mjs_is_foreign`, `mjs_is_function`, `mjs_is_null`, `mjs_is_number`, `mjs_is_object`, `mjs_is_string`, `mjs_is_undefined`, `mjs_mk_array`, `mjs_mk_array_buf`, `mjs_mk_boolean`, `mjs_mk_foreign`, `mjs_mk_null`, `mjs_mk_number`, `mjs_mk_object`, `mjs_mk_string`, `mjs_mk_undefined`, `mjs_nargs`, `mjs_next`, `mjs_own`, `mjs_prepend_errorf`, `mjs_return`, `mjs_set`, `mjs_set_errorf`, `mjs_set_exec_flags_poller`, `mjs_set_ffi_resolver`, `mjs_sprintf`, `mjs_strcmp`, `mjs_strerror`, `mjs_to_string`.

### SimpleArray (2)

No guest SimpleArray producer or handle exists, and the two exports do not identify element sizes/types needed to copy data safely.

`simple_array_cget_data`, `simple_array_get_count`.

### Variadic libc (2)

ARM varargs are not marshalled to native varargs.

`snprintf`, `sscanf`.

## Validation

- ESP-IDF firmware builds passed after libc, toolbox, GUI, buffer/storage, utility and
  ctype batches. The only runtime warning was the pre-existing deprecated
  `view_dispatcher_enable_queue` call.
- Rebuilt the host DLL after the final import-list change. The C-versus-Unicorn suite
  passed both RPS fixtures, DVD replay (8,839 calls / 2,200 frames), instruction/flag
  comparisons, memory protection, malformed ELF and budget checks.
- `tests/host/test_arm_fap_other.py` compiles the actual new libc, bit-field and
  BitBuffer switch cases with the real VM/native BitBuffer code. It passed pointer
  translation, read-only destinations, arena-edge strings, overlap rejection,
  realloc preservation/failure, integer and float ABI results, guest abort isolation,
  invalid handles, append overflow, 12,160 bit-field cases, and ctype ELF data relocation.
- No board was flashed. GUI/native callback scheduling, storage behavior, and hardware
  execution of FAPs using the new imports remain unverified.

## Added symbols

`__assert_func`, `__errno`, `_ctype_`, `abort`, `args_read_int_and_trim`, `args_read_probably_quoted_string_and_trim`, `args_read_string_and_trim`, `atoi`, `bit_buffer_alloc`, `bit_buffer_append_byte`, `bit_buffer_append_bytes`, `bit_buffer_free`, `bit_buffer_get_data`, `bit_buffer_get_size_bytes`, `bit_buffer_reset`, `bit_lib_bytes_to_num_bcd`, `bit_lib_bytes_to_num_be`, `bit_lib_bytes_to_num_le`, `bit_lib_get_bits`, `bit_lib_get_bits_16`, `bit_lib_get_bits_32`, `bit_lib_get_bits_64`, `bit_lib_num_to_bytes_be`, `buffered_file_stream_alloc`, `buffered_file_stream_close`, `buffered_file_stream_open`, `byte_input_alloc`, `byte_input_free`, `byte_input_get_view`, `byte_input_set_header_text`, `byte_input_set_result_callback`, `calloc`, `datetime_datetime_to_timestamp`, `datetime_get_days_per_month`, `datetime_get_days_per_year`, `datetime_is_leap_year`, `datetime_timestamp_to_datetime`, `dir_walk_alloc`, `dir_walk_free`, `dir_walk_open`, `dir_walk_read`, `dir_walk_set_recursive`, `dolphin_deed`, `file_stream_alloc`, `file_stream_close`, `file_stream_open`, `hex_char_to_uint8`, `loading_alloc`, `loading_free`, `loading_get_view`, `locale_celsius_to_fahrenheit`, `locale_fahrenheit_to_celsius`, `locale_format_date`, `locale_format_time`, `locale_get_date_format`, `locale_get_time_format`, `manchester_advance`, `memchr`, `memcmp`, `memmgr_get_free_heap`, `menu_add_item`, `menu_alloc`, `menu_free`, `menu_get_view`, `menu_reset`, `number_input_alloc`, `number_input_free`, `number_input_get_view`, `number_input_set_header_text`, `number_input_set_result_callback`, `path_extract_extension`, `path_extract_filename`, `path_extract_filename_no_ext`, `popup_alloc`, `popup_disable_timeout`, `popup_enable_timeout`, `popup_free`, `popup_get_view`, `popup_reset`, `popup_set_callback`, `popup_set_context`, `popup_set_header`, `popup_set_text`, `popup_set_timeout`, `pretty_format_bytes_hex_canonical`, `random`, `realloc`, `roundf`, `saved_struct_load`, `saved_struct_save`, `srand`, `strcasecmp`, `strchr`, `strcpy`, `strdup`, `strint_to_uint32`, `strncasecmp`, `strncmp`, `strncpy`, `strrchr`, `strstr`, `strtof`, `strtol`, `strtoul`, `strtoull`, `value_index_uint32`.
