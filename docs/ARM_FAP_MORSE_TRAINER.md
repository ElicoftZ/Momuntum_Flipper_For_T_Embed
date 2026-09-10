# Morse Trainer compatibility check — 2026-09-05

Morse Trainer is not supported by the current ARM translator. The device twice
rejected `/ext/apps/ARM/morse_trainer.fap` before starting its app thread:
`Unsupported ARM import: strlcat`. This is an import-resolution failure; the
capture does not show a crash or an out-of-memory failure. The launcher then
returned to the browser because `loader_start_with_gui_error` only logged its
error. It now displays the missing API, dismissible with OK or Back.

## Evidence

- Device capture: `build_host/morse_launch.log` (timestamps 182650 and 184089).
- Original catalog FAP: 32,892 bytes, API 88.2, target 7.
- Catalog version ID: `6a5d075b63013e0edb1bf9d6`.
- SHA-256: `0153ea83747017c491540fb9547f03d8b0f357f9f98eac634664683b7a64e8e5`.
- The actual C interpreter reproduces `Unsupported ARM import: strlcat` when
  loading this unchanged catalog binary on the host.
- ELF symbol table: 113 unique undefined symbols, 27 present in the current
  bridge list and 86 absent. Presence alone does not prove compatible runtime
  behavior. The installed file was not copied back, so its complete hash has
  not been compared with the catalog artifact.
- Allocated sections: text 10,156 bytes, rodata 2,444 bytes, bss 8 bytes. Heap,
  worker stacks and native objects are additional; this is not a peak RAM test.
- Source inspected at commit `94ee91cfe1247640e3c36ddf0f47b473a06f5af4`:
  https://github.com/barismert98/flipper-morse-trainer/tree/94ee91cfe1247640e3c36ddf0f47b473a06f5af4

## Required translation work

1. Bounded memory/string functions and ARM variadic argument handling, with
   guest-pointer and buffer-size tests. Returning fake success for missing
   imports would let the program proceed with invalid state.
2. Scene-manager callbacks and menu/widget/variable-item handles, custom events,
   guest strings, additional drawing functions, and SD progress/settings I/O.
3. Independent guest thread/register/stack state, queues, timer callbacks and
   shutdown/join behavior. The sound worker blocks while the UI continues, so
   the existing single execution context cannot simply be shared across both.
4. Speaker ownership, float argument marshaling, RTC timestamps, and notification
   sequence data. Flipper pointers and notification layouts cannot be passed
   directly into ESP32 APIs.
5. A control mapping using T-Embed's actual Up/Down/OK/Back inputs. Up and Down
   already mean replay/hint; Left and Right choose decoding answers, Left clears
   encoding and Right advances teaching. Replacing Up/Down globally would lose
   existing functions. OK press/release timing must remain intact for dots/dashes.
6. Run the unchanged FAP through teaching, encode/decode, practice, settings,
   saving/reloading and repeated exit/reopen before claiming support.

The diagnostic firmware change does not implement these 86 missing symbols or
make Morse Trainer runnable. The earlier folder freeze is a separate hardware
regression still requiring validation with its affected files.

## Missing symbols in this catalog binary
__wrap_snprintf
canvas_draw_box
canvas_draw_frame
canvas_draw_line
canvas_draw_str
furi_delay_ms
furi_get_tick
furi_hal_rtc_get_timestamp
furi_hal_speaker_acquire
furi_hal_speaker_release
furi_hal_speaker_start
furi_hal_speaker_stop
furi_message_queue_alloc
furi_message_queue_free
furi_message_queue_get
furi_message_queue_put
furi_message_queue_reset
furi_ms_to_ticks
furi_string_alloc
furi_string_cat_printf
furi_string_cat_str
furi_string_free
furi_string_get_cstr
furi_thread_alloc_ex
furi_thread_free
furi_thread_join
furi_thread_start
furi_timer_alloc
furi_timer_free
furi_timer_is_running
furi_timer_restart
furi_timer_start
furi_timer_stop
memcpy
memmove
memset
notification_message
scene_manager_alloc
scene_manager_free
scene_manager_handle_back_event
scene_manager_handle_custom_event
scene_manager_next_scene
scene_manager_search_and_switch_to_previous_scene
sequence_blink_green_10
sequence_blink_red_10
sequence_reset_rgb
sequence_reset_vibro
sequence_set_only_blue_255
sequence_set_vibro_on
storage_common_mkdir
storage_file_alloc
storage_file_close
storage_file_free
storage_file_open
storage_file_read
storage_file_write
strcmp
strlcat
strlcpy
strlen
submenu_add_item
submenu_alloc
submenu_free
submenu_get_view
submenu_reset
submenu_set_header
submenu_set_selected_item
variable_item_get_context
variable_item_get_current_value_index
variable_item_list_add
variable_item_list_alloc
variable_item_list_free
variable_item_list_get_view
variable_item_list_reset
variable_item_list_set_enter_callback
variable_item_set_current_value_index
variable_item_set_current_value_text
view_dispatcher_send_custom_event
view_dispatcher_set_custom_event_callback
widget_add_button_element
widget_add_string_element
widget_add_text_scroll_element
widget_alloc
widget_free
widget_get_view
widget_reset

## Diagnostic firmware validation

Firmware build and partition checks passed (`build_host/morse_error_build.log`).
App-only flash to COM4 at 0x20000 succeeded and esptool verified the data
(`build_host/morse_error_flash.log`). Binary size: 3,305,088 bytes. SHA-256:
`aba2ba396402d46bab838e8065feee297e56828eab76702dbcdde20a979c0380`.

Boot completed with 22,043 bytes internal free and a 14,336-byte largest block.
The user confirmed that the missing-API message is visible. The affected folder
was not explicitly confirmed; capture: `build_host/morse_error_hardware.log`.

## First shared API extension — 2026-09-06

Implemented `memcpy`, `memmove`, `memset`, `strlen`, `strcmp`, `strlcpy` and
`strlcat` in the portable interpreter. The historical list above is the original
86-symbol gap; these seven now resolve, leaving 79 absent symbols in the same
catalog binary. Both host loading and the user's hardware test reach
`Unsupported ARM import: furi_delay_ms` next. This does not mean Morse Trainer
is runnable yet.

Actual memory/string imports pass guest-trap tests, including 256 randomized
truncation/guard cases. The two unchanged RPS binaries still pass their 357-call
C-versus-Unicorn traces and 2,464 instruction/flag comparisons. Firmware build,
partition checks and app-only flash succeeded; esptool verified the data.

- Binary: 3,305,680 bytes; SHA-256
  `b2043792c931187700308f6e5dd1197b19b46b95a7f0810476c0ef20f45844cf`.
- Guest arena remains 64 KiB, with no additional persistent libc buffers.
- Boot internal free: 22,171 bytes; largest free block: 14,336 bytes.
- Logs: `build_host/arm_libc_tests.log`, `arm_libc_build.log`,
  `arm_libc_flash.log`, `arm_libc_hardware.log`.
