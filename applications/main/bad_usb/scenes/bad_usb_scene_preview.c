#include "../bad_usb_app_i.h"
#include "../helpers/ducky_script.h"
#include "../helpers/ducky_script_i.h"
#include <stdio.h>
#include <string.h>

#define PREVIEW_LINE_MAX 96

enum {
    PreviewEventPrevious = 0x7100,
    PreviewEventNext,
    PreviewEventContinue,
};

typedef struct {
    uint16_t line_count;
    uint16_t action_count;
    uint16_t error_line;
    uint16_t layout_error_line;
    uint16_t current_line;
    uint32_t estimated_ms;
    bool layout_ok;
    bool line_too_long;
    char line[PREVIEW_LINE_MAX];
} BadUsbPreview;

static BadUsbPreview s_preview;
static uint16_t s_layout_map[128];

static void preview_save_recent_path(BadUsbApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(
        file, BAD_USB_APP_BASE_FOLDER "/.recent", FSAM_WRITE, FSOM_CREATE_ALWAYS);
    if(ok) {
        const char* path = furi_string_get_cstr(app->file_path);
        const size_t length = strlen(path);
        storage_file_write(file, path, length);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void preview_button_cb(GuiButtonType result, InputType type, void* context) {
    if(type != InputTypeShort) return;
    BadUsbApp* app = context;
    uint32_t event = 0;
    if(result == GuiButtonTypeLeft) event = PreviewEventPrevious;
    else if(result == GuiButtonTypeCenter) event = PreviewEventNext;
    else if(result == GuiButtonTypeRight) event = PreviewEventContinue;
    if(event) view_dispatcher_send_custom_event(app->view_dispatcher, event);
}

static bool preview_is_command(const char* name, size_t length) {
    static const char* const commands[] = {
        "REM", "ID", "BT_ID", "BLE_ID", "DELAY", "STRING", "STRINGLN",
        "DEFAULT_DELAY", "DEFAULTDELAY", "STRINGDELAY", "STRING_DELAY",
        "DEFAULT_STRING_DELAY", "DEFAULTSTRINGDELAY", "REPEAT", "SYSRQ",
        "ALTCHAR", "ALTSTRING", "ALTCODE", "HOLD", "RELEASE",
        "WAIT_FOR_BUTTON_PRESS", "MEDIA", "GLOBE", "MOUSEMOVE", "MOUSE_MOVE",
        "MOUSESCROLL", "MOUSE_SCROLL",
    };
    for(size_t i = 0; i < COUNT_OF(commands); ++i) {
        if(strlen(commands[i]) == length && strncmp(name, commands[i], length) == 0) return true;
    }
    return false;
}

static bool preview_line_supported(const char* line, uint32_t* delay_ms) {
    while(*line == ' ' || *line == '\t') line++;
    if(!line[0]) return true;

    const size_t word_length = strcspn(line, " \t");
    if(preview_is_command(line, word_length)) {
        if((word_length == 5 && strncmp(line, "DELAY", 5) == 0) ||
           (word_length == 6 && strncmp(line, "REPEAT", 6) == 0) ||
           (word_length == 13 && strncmp(line, "DEFAULT_DELAY", 13) == 0) ||
           (word_length == 12 && strncmp(line, "DEFAULTDELAY", 12) == 0)) {
            const char* value = line + word_length;
            while(*value == ' ' || *value == '\t') value++;
            uint32_t parsed = 0;
            if(!ducky_get_number(value, &parsed)) return false;
            if(word_length == 5 && parsed == 0) return false;
            if(word_length == 6 && parsed == 0) return false;
            if(word_length == 5) *delay_ms += parsed;
        }
        if((word_length == 6 && strncmp(line, "STRING", 6) == 0) ||
           (word_length == 8 && strncmp(line, "STRINGLN", 8) == 0)) {
            const char* value = line + word_length;
            while(*value == ' ' || *value == '\t') value++;
            *delay_ms += (uint32_t)(strlen(value) * 12U);
        }
        return true;
    }

    const char* key = line;
    while(true) {
        const char* before = key;
        if(ducky_get_next_modifier_keycode_by_name(&key) == HID_KEYBOARD_NONE) break;
        if(key == before) break;
        while(*key == ' ' || *key == '-') key++;
    }
    if(ducky_get_keycode_by_name(key) != HID_KEYBOARD_NONE) return true;
    if(ducky_get_mouse_keycode_by_name(key) != HID_MOUSE_INVALID) return true;
    /* Single printable keys use the active layout; command-like words must
     * match the parser's named-key table or a supported directive above. */
    return key[0] != '\0' && key[1] == '\0';
}

static bool preview_line_matches_layout(const char* line) {
    while(*line == ' ' || *line == '\t') line++;
    const size_t word_length = strcspn(line, " \t");
    const bool is_string =
        (word_length == 6 && strncmp(line, "STRING", word_length) == 0) ||
        (word_length == 8 && strncmp(line, "STRINGLN", word_length) == 0);
    if(is_string) {
        const char* text = line + word_length;
        while(*text == ' ' || *text == '\t') text++;
        while(*text) {
            const uint8_t character = (uint8_t)*text++;
            if(character == '\n' || character == '\r') continue;
            if(character >= 128 || s_layout_map[character] == HID_KEYBOARD_NONE) return false;
        }
        return true;
    }

    if(preview_is_command(line, word_length)) return true;
    const char* key = line;
    while(true) {
        const char* before = key;
        if(ducky_get_next_modifier_keycode_by_name(&key) == HID_KEYBOARD_NONE) break;
        if(key == before) break;
        while(*key == ' ' || *key == '-') key++;
    }
    if(ducky_get_keycode_by_name(key) != HID_KEYBOARD_NONE ||
       ducky_get_mouse_keycode_by_name(key) != HID_MOUSE_INVALID) {
        return true;
    }
    return key[0] && key[1] == '\0' && (uint8_t)key[0] < 128 &&
           s_layout_map[(uint8_t)key[0]] != HID_KEYBOARD_NONE;
}

static bool preview_read_layout(BadUsbApp* app) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    FileInfo info;
    const char* path = furi_string_get_cstr(app->keyboard_layout);
    bool ok = storage_file_open(
        file,
        path,
        FSAM_READ,
        FSOM_OPEN_EXISTING);
    if(ok) {
        ok = storage_common_stat(storage, path, &info) == FSE_OK &&
             info.size == sizeof(s_layout_map) &&
             storage_file_read(file, s_layout_map, sizeof(s_layout_map)) == sizeof(s_layout_map);
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

static void preview_process_line(const char* source, uint16_t line_number) {
    char line[PREVIEW_LINE_MAX];
    size_t length = strlen(source);
    if(length >= sizeof(line)) length = sizeof(line) - 1;
    memcpy(line, source, length);
    line[length] = '\0';
    while(length && (line[length - 1] == '\r' || line[length - 1] == ' ' ||
                     line[length - 1] == '\t')) {
        line[--length] = '\0';
    }

    if(line_number == s_preview.current_line) {
        strlcpy(s_preview.line, line, sizeof(s_preview.line));
    }
    const char* first = line;
    while(*first == ' ' || *first == '\t') first++;
    if(!first[0]) return;
    if(strncmp(first, "REM", 3) == 0 && (first[3] == '\0' || first[3] == ' ' || first[3] == '\t'))
        return;

    s_preview.action_count++;
    if(!preview_line_supported(first, &s_preview.estimated_ms) && !s_preview.error_line) {
        s_preview.error_line = line_number;
    }
    if(!s_preview.layout_error_line && !preview_line_matches_layout(first)) {
        s_preview.layout_error_line = line_number;
    }
}

static void preview_read_script(BadUsbApp* app) {
    memset(&s_preview, 0, sizeof(s_preview));
    s_preview.current_line = 1;
    s_preview.layout_ok = preview_read_layout(app);
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    if(!storage_file_open(
           file,
           furi_string_get_cstr(app->file_path),
           FSAM_READ,
           FSOM_OPEN_EXISTING)) {
        s_preview.error_line = 1;
        strlcpy(s_preview.line, "Could not read script", sizeof(s_preview.line));
    } else {
        preview_save_recent_path(app);
        char input[128];
        char line[PREVIEW_LINE_MAX];
        size_t line_length = 0;
        uint16_t line_number = 1;
        size_t bytes = 0;
        bool too_long = false;
        while((bytes = storage_file_read(file, input, sizeof(input))) > 0) {
            for(size_t i = 0; i < bytes; ++i) {
                if(input[i] == '\n') {
                    line[line_length] = '\0';
                    s_preview.current_line = line_number;
                    preview_process_line(line, line_number);
                    if(too_long && !s_preview.error_line) s_preview.error_line = line_number;
                    s_preview.line_count++;
                    line_number++;
                    line_length = 0;
                    too_long = false;
                } else if(line_length + 1 < sizeof(line)) {
                    line[line_length++] = input[i];
                } else {
                    too_long = true;
                }
            }
        }
        if(line_length > 0) {
            line[line_length] = '\0';
            s_preview.current_line = line_number;
            preview_process_line(line, line_number);
            if(too_long && !s_preview.error_line) s_preview.error_line = line_number;
            s_preview.line_count++;
        }
        s_preview.current_line = 1;
        if(s_preview.line_count == 0) s_preview.error_line = 1;
        storage_file_close(file);
    }
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void preview_refresh_current_line(BadUsbApp* app) {
    uint16_t wanted = s_preview.current_line;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);
    s_preview.line[0] = '\0';
    if(storage_file_open(
           file,
           furi_string_get_cstr(app->file_path),
           FSAM_READ,
           FSOM_OPEN_EXISTING)) {
        uint16_t current = 1;
        char input[128];
        size_t length = 0;
        size_t bytes = 0;
        while((bytes = storage_file_read(file, input, sizeof(input))) > 0) {
            for(size_t i = 0; i < bytes; ++i) {
                if(input[i] == '\n') {
                    if(current == wanted) {
                        s_preview.line[length] = '\0';
                        goto done;
                    }
                    current++;
                    length = 0;
                } else if(current == wanted && length + 1 < sizeof(s_preview.line)) {
                    s_preview.line[length++] = input[i];
                }
            }
        }
        if(current == wanted) s_preview.line[length] = '\0';
    }
done:
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
}

static void preview_show(BadUsbApp* app) {
    preview_refresh_current_line(app);
    char text[160];
    if(s_preview.error_line) {
        snprintf(
            text,
            sizeof(text),
            "%u lines  %u actions\nCheck line %u\n%u: %.64s",
            (unsigned)s_preview.line_count,
            (unsigned)s_preview.action_count,
            (unsigned)s_preview.error_line,
            (unsigned)s_preview.current_line,
            s_preview.line);
    } else if(s_preview.layout_error_line) {
        snprintf(
            text,
            sizeof(text),
            "%u lines  %u actions\nLayout issue at %u\n%u: %.64s",
            (unsigned)s_preview.line_count,
            (unsigned)s_preview.action_count,
            (unsigned)s_preview.layout_error_line,
            (unsigned)s_preview.current_line,
            s_preview.line);
    } else if(!s_preview.layout_ok) {
        snprintf(text, sizeof(text), "Keyboard layout unavailable\nChoose a valid 256-byte layout");
    } else {
        snprintf(
            text,
            sizeof(text),
            "%u lines  %u actions\nAbout %u sec\n%u: %.64s",
            (unsigned)s_preview.line_count,
            (unsigned)s_preview.action_count,
            (unsigned)(s_preview.estimated_ms / 1000U),
            (unsigned)s_preview.current_line,
            s_preview.line);
    }
    widget_reset(app->widget);
    widget_add_string_element(
        app->widget, 64, 8, AlignCenter, AlignBottom, FontPrimary, "Preview: no keys sent");
    widget_add_text_box_element(app->widget, 0, 13, 128, 47, AlignLeft, AlignTop, text, false);
    widget_add_button_element(
        app->widget, GuiButtonTypeLeft, "Prev", preview_button_cb, app);
    widget_add_button_element(
        app->widget, GuiButtonTypeCenter, "Next", preview_button_cb, app);
    if(!s_preview.error_line && !s_preview.layout_error_line && s_preview.layout_ok) {
        widget_add_button_element(
            app->widget, GuiButtonTypeRight, "Continue", preview_button_cb, app);
    }
    view_dispatcher_switch_to_view(app->view_dispatcher, BadUsbAppViewWidget);
}

void bad_usb_scene_preview_on_enter(void* context) {
    BadUsbApp* app = context;
    preview_read_script(app);
    preview_show(app);
}

bool bad_usb_scene_preview_on_event(void* context, SceneManagerEvent event) {
    BadUsbApp* app = context;
    if(event.type != SceneManagerEventTypeCustom) return false;
    if(event.event == PreviewEventPrevious) {
        if(s_preview.current_line > 1) s_preview.current_line--;
        preview_show(app);
        return true;
    }
    if(event.event == PreviewEventNext) {
        if(s_preview.current_line < s_preview.line_count) s_preview.current_line++;
        preview_show(app);
        return true;
    }
    if(event.event == PreviewEventContinue && !s_preview.error_line) {
        scene_manager_next_scene(app->scene_manager, BadUsbSceneWork);
        return true;
    }
    return false;
}

void bad_usb_scene_preview_on_exit(void* context) {
    UNUSED(context);
}
