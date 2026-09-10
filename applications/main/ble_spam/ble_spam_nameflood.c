#include "ble_spam_nameflood.h"

#include <furi.h>
#include <storage/storage.h>
#include <toolbox/stream/file_stream.h>

#define BLE_SPAM_NAMEFLOOD_PATH EXT_PATH("ble/nameflood.txt")
#define BLE_SPAM_NAME_MAX       20
#define BLE_SPAM_NAME_COUNT_MAX 64

static const char* default_names[] = {
    "Bluetooth Device",
    "Wireless Keyboard",
    "Wireless Mouse",
    "Game Controller",
};

static char file_names[BLE_SPAM_NAME_COUNT_MAX][BLE_SPAM_NAME_MAX];
static size_t file_name_count = 0;

void ble_spam_nameflood_reload(void) {
    file_name_count = 0;

    Storage* storage = furi_record_open(RECORD_STORAGE);
    Stream* stream = file_stream_alloc(storage);
    FuriString* line = furi_string_alloc();

    if(file_stream_open(stream, BLE_SPAM_NAMEFLOOD_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        while(file_name_count < BLE_SPAM_NAME_COUNT_MAX && stream_read_line(stream, line)) {
            furi_string_replace_all(line, "\r", "");
            furi_string_replace_all(line, "\n", "");
            if(furi_string_size(line) > 0) {
                strlcpy(
                    file_names[file_name_count],
                    furi_string_get_cstr(line),
                    sizeof(file_names[file_name_count]));
                file_name_count++;
            }
        }
        file_stream_close(stream);
    }

    furi_string_free(line);
    stream_free(stream);
    furi_record_close(RECORD_STORAGE);
}

size_t ble_spam_nameflood_name_count(void) {
    if(file_name_count > 0) return file_name_count;
    return sizeof(default_names) / sizeof(default_names[0]);
}

const char* ble_spam_nameflood_name_at(size_t index) {
    if(file_name_count > 0) return file_names[index % file_name_count];
    return default_names[index % (sizeof(default_names) / sizeof(default_names[0]))];
}
