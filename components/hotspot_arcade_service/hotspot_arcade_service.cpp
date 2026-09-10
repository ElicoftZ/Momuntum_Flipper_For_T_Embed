#include "hotspot_arcade_service.h"

#include <hotspot_arcade_runtime/hotspot_arcade_runtime.h>

#include <Arduino.h>
#include "vendor/engine/ha_games.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <new>
#include <sys/stat.h>

#include <furi.h>
#include <storage/storage.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <furi_hal_power.h>
#include <btshim.h>
#include <freertos/idf_additions.h>
#include <notification/notification_messages.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

namespace {

constexpr size_t kPathSize = 384;
constexpr size_t kConsoleLines = 32;
constexpr size_t kConsoleLineSize = 80;
constexpr size_t kMaxPackCandidates = 32;
constexpr size_t kMaxPackBytes = 128U * 1024U;
constexpr size_t kMaxWsMessageSize = 512;
constexpr uint32_t kTickPeriodMs = 10;
constexpr uint32_t kTickTaskStackSize = 8192;
constexpr UBaseType_t kTickTaskPriority = 5;

enum EngineCommand : uint8_t {
    EngineCommandNone = 0,
    EngineCommandSelectGame,
    EngineCommandResetScores,
    EngineCommandRoundEnd,
};

constexpr const char* kTag = "ha_service";
constexpr const char* kApIp = "192.168.4.1";

struct MirrorPlayer {
    bool used;
    char nick[HA_NICK_LEN];
    int32_t score;
};

struct ServiceState {
    SemaphoreHandle_t engine_mutex;
    SemaphoreHandle_t tick_done;
    SemaphoreHandle_t command_done;
    TaskHandle_t tick_task;
    Engine* engine;
    void* engine_storage;

    std::atomic<bool> accepting;
    std::atomic<bool> runtime_started;
    std::atomic<bool> tick_stop;
    std::atomic<uint8_t> command;
    /* A phone vote is received on the HTTP server task. That task must never
     * reload packs or reset a game: both are stack-heavy and switching after a
     * played round overflowed/corrupted the small httpd stack. Zero means no
     * pending phone request; real games are numbered 1..HA_GAME_TUGOFWAR. */
    std::atomic<uint8_t> phone_game;
    uint8_t command_game;
    HotspotArcadeServiceResult command_result;

    char ssid[33];
    char bundled_packs_dir[kPathSize];
    char user_packs_dir[kPathSize];
    char art_dir[kPathSize];
    char current_art_path[kPathSize];
    char lang[8];

    MirrorPlayer players[HA_MAX_PLAYERS + 1];
    char console[kConsoleLines][kConsoleLineSize];
    uint32_t console_total;
    char last_event[kConsoleLineSize];
    uint8_t active_game;
    uint32_t revision;

    File* art_file;
    /* Sticky: any short write during the drawing makes the finished SVG
     * incomplete, and art_end() must report that rather than claim success. */
    bool art_write_error;
    uint32_t art_sequence;
};

struct PackCandidate {
    char name[96];
    char path[kPathSize];
};

/* True while the session holds the display/sleep locks. Declared here because
 * hotspot_arcade_service_start() sets it well before release_wake_lock(). */
static bool g_wake_held = false;
/* True only when WE turned Bluetooth off, so stop() does not turn it on for
 * a user who already had it off. Frees the ~60 KB the BLE controller holds
 * in internal DRAM so WiFi softAP has DMA-capable memory to attach. */
static bool g_bt_disabled_by_us = false;

std::atomic<ServiceState*> g_state{nullptr};
StaticSemaphore_t g_api_mutex_storage;

SemaphoreHandle_t api_mutex() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&g_api_mutex_storage);
    return mutex;
}

class ApiGuard {
public:
    ApiGuard() : mutex_(api_mutex()) {
        if(mutex_) xSemaphoreTake(mutex_, portMAX_DELAY);
    }

    ~ApiGuard() {
        if(mutex_) xSemaphoreGive(mutex_);
    }

    ApiGuard(const ApiGuard&) = delete;
    ApiGuard& operator=(const ApiGuard&) = delete;

private:
    SemaphoreHandle_t mutex_;
};

class EngineGuard {
public:
    explicit EngineGuard(ServiceState* state) : state_(state), locked_(false) {
        if(state_ && state_->engine_mutex) {
            locked_ = xSemaphoreTakeRecursive(state_->engine_mutex, portMAX_DELAY) == pdTRUE;
        }
    }

    ~EngineGuard() {
        if(locked_) xSemaphoreGiveRecursive(state_->engine_mutex);
    }

    bool locked() const {
        return locked_;
    }

    EngineGuard(const EngineGuard&) = delete;
    EngineGuard& operator=(const EngineGuard&) = delete;

private:
    ServiceState* state_;
    bool locked_;
};

size_t safe_copy(char* destination, const char* source, size_t capacity) {
    if(!destination || capacity == 0) return source ? strlen(source) : 0;
    const char* value = source ? source : "";
    const size_t length = strlen(value);
    const size_t copied = length < capacity - 1 ? length : capacity - 1;
    if(copied) memcpy(destination, value, copied);
    destination[copied] = '\0';
    return length;
}

bool copy_checked(char* destination, size_t capacity, const char* source) {
    if(!destination || !capacity || !source) return false;
    return safe_copy(destination, source, capacity) < capacity;
}

void mirror_touch(ServiceState* state) {
    if(state) state->revision++;
}

void log_line(ServiceState* state, const char* line) {
    if(!state || !line) return;
    safe_copy(
        state->console[state->console_total % kConsoleLines],
        line,
        sizeof(state->console[0]));
    state->console_total++;
    mirror_touch(state);
    ESP_LOGI(kTag, "%s", line);
}

void log_format(ServiceState* state, const char* format, ...) {
    char line[kConsoleLineSize];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    log_line(state, line);
}

void set_last_event(ServiceState* state, const char* event) {
    if(!state || !event) return;
    safe_copy(state->last_event, event, sizeof(state->last_event));
    log_line(state, event);
}

int ascii_case_compare(const char* lhs, const char* rhs) {
    while(*lhs && *rhs) {
        unsigned char a = static_cast<unsigned char>(*lhs++);
        unsigned char b = static_cast<unsigned char>(*rhs++);
        if(a >= 'A' && a <= 'Z') a = static_cast<unsigned char>(a + ('a' - 'A'));
        if(b >= 'A' && b <= 'Z') b = static_cast<unsigned char>(b + ('a' - 'A'));
        if(a != b) return a < b ? -1 : 1;
    }
    if(*lhs == *rhs) return 0;
    return *lhs ? 1 : -1;
}

bool has_txt_extension(const char* name) {
    if(!name) return false;
    const size_t length = strlen(name);
    return length > 4 && ascii_case_compare(name + length - 4, ".txt") == 0;
}

bool join_path(char* out, size_t out_size, const char* left, const char* right) {
    if(!out || !out_size || !left || !right) return false;
    const size_t left_length = strlen(left);
    const bool has_separator = left_length > 0 && left[left_length - 1] == '/';
    const int written = snprintf(out, out_size, has_separator ? "%s%s" : "%s/%s", left, right);
    return written >= 0 && static_cast<size_t>(written) < out_size;
}

bool map_storage_path(const char* input, char* output, size_t output_size) {
    if(!input || !input[0] || !output || output_size == 0) return false;
    /* Stay in the Furi storage namespace. Every consumer of these paths --
     * path_is_directory/ensure_directory here, storage_file_open in the
     * runtime -- goes through the storage API, which resolves "/ext" and
     * "/int" and maps them internally.
     *
     * This used to rewrite "/ext/..." to "/sdcard/...", which was correct back
     * when those consumers were POSIX. After they moved to the storage API the
     * rewrite became the bug: the FAP passed a valid /ext path, this turned it
     * into a namespace the storage API cannot resolve, and Start Session died
     * with a bare "storage error" on a card that had the files. Nothing is
     * mounted behind /sdcard on this port -- see the comment below. */
    int written;
    if(strncmp(input, "/any", 4) == 0) {
        /* Normalise to the concrete namespace the assets actually live in. */
        written = snprintf(output, output_size, "/ext%s", input + 4);
    } else {
        written = snprintf(output, output_size, "%s", input);
    }
    if(written < 0 || static_cast<size_t>(written) >= output_size) return false;

    size_t length = strlen(output);
    while(length > 1 && output[length - 1] == '/') output[--length] = '\0';
    return output[0] == '/';
}

/* This port does NOT register the SD card with ESP-IDF's VFS -- the storage
 * service drives FatFs directly (f_open/f_opendir), so nothing is mounted
 * behind "/sdcard" and POSIX stat/mkdir/fopen/opendir can never succeed here.
 * All filesystem access therefore goes through the Furi storage API, which
 * takes "/ext/..." paths and maps them internally. Paths handed to this
 * service must be in the /ext namespace for the same reason. */
bool path_is_directory(const char* path) {
    if(!path || !path[0]) return false;
    Storage* storage = static_cast<Storage*>(furi_record_open(RECORD_STORAGE));
    const bool exists = storage_dir_exists(storage, path);
    furi_record_close(RECORD_STORAGE);
    return exists;
}

bool ensure_directory(const char* path) {
    if(!path || path[0] != '/') return false;
    char scratch[kPathSize];
    if(!copy_checked(scratch, sizeof(scratch), path)) return false;

    Storage* storage = static_cast<Storage*>(furi_record_open(RECORD_STORAGE));
    /* storage_simply_mkdir() also returns true when the directory already
     * exists, so walking the components gives mkdir -p semantics. */
    for(char* cursor = scratch + 1; *cursor; cursor++) {
        if(*cursor != '/') continue;
        *cursor = '\0';
        storage_simply_mkdir(storage, scratch);
        *cursor = '/';
    }
    storage_simply_mkdir(storage, scratch);
    const bool ok = storage_dir_exists(storage, scratch);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

bool derive_art_directory(const char* user_packs_dir, char* output, size_t output_size) {
    if(!copy_checked(output, output_size, user_packs_dir)) return false;
    char* last_separator = strrchr(output, '/');
    if(!last_separator || last_separator == output) return false;
    *last_separator = '\0';
    const size_t parent_length = strlen(output);
    const int written = snprintf(output + parent_length, output_size - parent_length, "/art");
    return written >= 0 && static_cast<size_t>(written) < output_size - parent_length;
}

const char* game_name(uint8_t game_id) {
    switch(game_id) {
    case HA_GAME_NONE:
        return "none";
    case HA_GAME_TRIVIA:
        return "trivia";
    case HA_GAME_CONNECT4:
        return "connect4";
    case HA_GAME_TICTACTOE:
        return "tictactoe";
    case HA_GAME_DOTS:
        return "dots";
    case HA_GAME_DRAW:
        return "draw";
    case HA_GAME_PONG:
        return "pong";
    case HA_GAME_REACT:
        return "react";
    case HA_GAME_WYR:
        return "wyr";
    case HA_GAME_SCRAMBLE:
        return "scramble";
    case HA_GAME_REVERSI:
        return "reversi";
    case HA_GAME_GUESSCOLOR:
        return "gc";
    case HA_GAME_BATTLESHIP:
        return "bs";
    case HA_GAME_SPECTRUM:
        return "spectrum";
    case HA_GAME_KMK:
        return "kmk";
    case HA_GAME_CHESS:
        return "chess";
    case HA_GAME_SECRETS:
        return "secrets";
    case HA_GAME_FILLBLANK:
        return "fillblank";
    case HA_GAME_WEREWOLF:
        return "werewolf";
    case HA_GAME_SPYFALL:
        return "spyfall";
    case HA_GAME_FRANKENDRAW:
        return "frankendraw";
    case HA_GAME_RPS:
        return "rps";
    case HA_GAME_MATHRUSH:
        return "math";
    case HA_GAME_SIMON:
        return "simon";
    case HA_GAME_IMPOSTOR:
        return "impostor";
    case HA_GAME_BULLS:
        return "bulls";
    case HA_GAME_2048:
        return "g2048";
    case HA_GAME_SNAKE:
        return "snake";
    case HA_GAME_MINES:
        return "mines";
    case HA_GAME_MEMORY:
        return "memory";
    case HA_GAME_PUZZLE15:
        return "p15";
    case HA_GAME_HILO:
        return "hilo";
    case HA_GAME_AIM:
        return "aim";
    case HA_GAME_ODDONE:
        return "oddone";
    case HA_GAME_DICE:
        return "dice";
    case HA_GAME_WORDBOMB:
        return "bomb";
    case HA_GAME_NIM:
        return "nim";
    case HA_GAME_GOMOKU:
        return "gomoku";
    case HA_GAME_CATEGORIES:
        return "cats";
    case HA_GAME_BIDWARS:
        return "bid";
    case HA_GAME_TUGOFWAR:
        return "tug";
    default:
        return "unknown";
    }
}

const char* content_game_directory(uint8_t game_id) {
    switch(game_id) {
    case HA_GAME_TRIVIA:
        return "trivia";
    case HA_GAME_DRAW:
        return "draw";
    case HA_GAME_WYR:
        return "wyr";
    case HA_GAME_SCRAMBLE:
        return "scramble";
    case HA_GAME_SPECTRUM:
        return "spectrum";
    case HA_GAME_KMK:
        return "kmk";
    case HA_GAME_SECRETS:
        return "secrets";
    case HA_GAME_FILLBLANK:
        return "fillblank";
    case HA_GAME_SPYFALL:
        return "spyfall";
    default:
        return nullptr;
    }
}

size_t pack_limit(uint8_t game_id) {
    return (game_id == HA_GAME_FILLBLANK || game_id == HA_GAME_SPYFALL) ? 3U : 8U;
}

bool build_game_directory(
    char* output,
    size_t output_size,
    const char* root,
    const char* game,
    const char* language) {
    char game_path[kPathSize];
    if(!join_path(game_path, sizeof(game_path), root, game)) return false;
    if(language && language[0]) return join_path(output, output_size, game_path, language);
    return copy_checked(output, output_size, game_path);
}

bool directory_has_pack(const char* path) {
    if(!path || !path[0]) return false;
    Storage* storage = static_cast<Storage*>(furi_record_open(RECORD_STORAGE));
    File* directory = storage_file_alloc(storage);
    bool found = false;

    if(storage_dir_open(directory, path)) {
        char name[kPathSize];
        FileInfo info;
        while(storage_dir_read(directory, &info, name, sizeof(name))) {
            if(info.flags & FSF_DIRECTORY) continue;
            if(has_txt_extension(name)) {
                found = true;
                break;
            }
        }
        storage_dir_close(directory);
    }

    storage_file_free(directory);
    furi_record_close(RECORD_STORAGE);
    return found;
}

bool add_pack_candidate(
    PackCandidate* candidates,
    size_t* count,
    const char* directory,
    const char* name,
    bool replace_existing) {
    if(!candidates || !count || !directory || !name) return false;
    char full_path[kPathSize];
    if(!join_path(full_path, sizeof(full_path), directory, name)) return false;
    {
        Storage* storage = static_cast<Storage*>(furi_record_open(RECORD_STORAGE));
        FileInfo info;
        const bool is_regular_file =
            storage_common_stat(storage, full_path, &info) == FSE_OK &&
            !(info.flags & FSF_DIRECTORY);
        furi_record_close(RECORD_STORAGE);
        /* Not a readable file is not an error: the caller skips it. */
        if(!is_regular_file) return true;
    }

    if(replace_existing) {
        for(size_t index = 0; index < *count; index++) {
            if(ascii_case_compare(candidates[index].name, name) != 0) continue;
            if(!copy_checked(candidates[index].path, sizeof(candidates[index].path), full_path))
                return false;
            return true;
        }
    }

    if(*count >= kMaxPackCandidates) return true;
    PackCandidate& candidate = candidates[*count];
    if(!copy_checked(candidate.name, sizeof(candidate.name), name) ||
       !copy_checked(candidate.path, sizeof(candidate.path), full_path))
        return false;
    (*count)++;
    return true;
}

bool enumerate_pack_directory(
    PackCandidate* candidates,
    size_t* count,
    const char* directory,
    bool replace_existing) {
    Storage* storage = static_cast<Storage*>(furi_record_open(RECORD_STORAGE));
    File* handle = storage_file_alloc(storage);

    if(!storage_dir_open(handle, directory)) {
        /* An absent directory is not a failure -- user packs are optional. */
        storage_file_free(handle);
        furi_record_close(RECORD_STORAGE);
        return true;
    }

    bool ok = true;
    char name[kPathSize];
    FileInfo info;
    while(storage_dir_read(handle, &info, name, sizeof(name))) {
        if(info.flags & FSF_DIRECTORY) continue;
        if(!has_txt_extension(name)) continue;
        if(!add_pack_candidate(candidates, count, directory, name, replace_existing)) {
            ok = false;
            break;
        }
    }

    storage_dir_close(handle);
    storage_file_free(handle);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

void sort_pack_candidates(PackCandidate* candidates, size_t count) {
    for(size_t index = 1; index < count; index++) {
        PackCandidate moving = candidates[index];
        size_t position = index;
        while(position > 0 && ascii_case_compare(candidates[position - 1].name, moving.name) > 0) {
            candidates[position] = candidates[position - 1];
            position--;
        }
        candidates[position] = moving;
    }
}

void trim_to(const char* begin, const char* end, String& output, bool lower = false) {
    while(begin < end && (*begin == ' ' || *begin == '\t' || *begin == '\r')) begin++;
    while(end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;
    output = "";
    output.reserve(static_cast<size_t>(end - begin) + 1);
    for(const char* cursor = begin; cursor < end; cursor++) {
        char value = *cursor;
        if(lower && value >= 'A' && value <= 'Z') value = static_cast<char>(value + ('a' - 'A'));
        output += value;
    }
}

void parse_content_pack(
    Engine& engine,
    uint8_t game_id,
    const char* text,
    const char* fallback_name) {
    String name = fallback_name;
    for(const char* line = text; line && *line;) {
        const char* end = strchr(line, '\n');
        if(!end) end = line + strlen(line);
        if(strncmp(line, "Pack:", 5) == 0) {
            String value;
            trim_to(line + 5, end, value);
            if(value.length()) name = value;
            break;
        }
        line = *end ? end + 1 : end;
    }
    engine.contentPack(game_id, name.c_str());

    String object = "{";
    String key;
    String value;
    bool any = false;
    for(const char* line = text; line && *line;) {
        const char* line_end = strchr(line, '\n');
        if(!line_end) line_end = line + strlen(line);
        const char* begin = line;
        const char* end = line_end;
        while(begin < end && (*begin == ' ' || *begin == '\t' || *begin == '\r')) begin++;
        while(end > begin && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r')) end--;

        const bool separator = begin == end ||
                               (end - begin == 3 && strncmp(begin, "---", 3) == 0);
        if(separator) {
            if(any) {
                object += "}";
                engine.contentItem(object.c_str());
            }
            object = "{";
            any = false;
        } else {
            const char* colon = static_cast<const char*>(
                memchr(begin, ':', static_cast<size_t>(end - begin)));
            if(colon) {
                trim_to(begin, colon, key, true);
                trim_to(colon + 1, end, value);
                if(key.length() && strcmp(key.c_str(), "pack") != 0) {
                    if(any) object += ",";
                    object += "\"";
                    object += ha_json_escape(key.c_str());
                    object += "\":\"";
                    object += ha_json_escape(value.c_str());
                    object += "\"";
                    any = true;
                }
            }
        }
        line = *line_end ? line_end + 1 : line_end;
    }
    if(any) {
        object += "}";
        engine.contentItem(object.c_str());
    }
}

bool load_pack_file(Engine& engine, uint8_t game_id, const PackCandidate& candidate) {
    Storage* storage = static_cast<Storage*>(furi_record_open(RECORD_STORAGE));
    File* file = storage_file_alloc(storage);

    if(!storage_file_open(file, candidate.path, FSAM_READ, FSOM_OPEN_EXISTING)) {
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    const uint64_t size64 = storage_file_size(file);
    if(size64 == 0 || size64 > kMaxPackBytes) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    const size_t size = static_cast<size_t>(size64);
    char* text = static_cast<char*>(
        heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if(!text) text = static_cast<char*>(heap_caps_malloc(size + 1, MALLOC_CAP_8BIT));
    if(!text) {
        storage_file_close(file);
        storage_file_free(file);
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    /* Chunked: a short storage_file_read() is normal for a large file, so read
     * until satisfied rather than treating the first partial read as failure. */
    size_t total = 0;
    while(total < size) {
        size_t chunk = size - total;
        if(chunk > 4096U) chunk = 4096U;
        const size_t read = storage_file_read(file, text + total, chunk);
        if(read == 0) break;
        total += read;
    }

    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(total != size) {
        heap_caps_free(text);
        return false;
    }
    text[size] = '\0';

    char fallback[sizeof(candidate.name)];
    safe_copy(fallback, candidate.name, sizeof(fallback));
    char* extension = strrchr(fallback, '.');
    if(extension) *extension = '\0';
    parse_content_pack(engine, game_id, text, fallback);
    heap_caps_free(text);
    return true;
}

bool load_content_for_game(ServiceState* state, uint8_t game_id) {
    if(!state || !state->engine) return false;
    state->engine->contentClear();
    const char* game_directory = content_game_directory(game_id);
    if(!game_directory) return true;

    PackCandidate* candidates = static_cast<PackCandidate*>(heap_caps_calloc(
        kMaxPackCandidates, sizeof(PackCandidate), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if(!candidates) {
        candidates = static_cast<PackCandidate*>(
            heap_caps_calloc(kMaxPackCandidates, sizeof(PackCandidate), MALLOC_CAP_8BIT));
    }
    if(!candidates) {
        log_line(state, "No memory for content index");
        return false;
    }

    char bundled_directory[kPathSize];
    char user_directory[kPathSize];
    const bool translated = state->lang[0] && strcmp(state->lang, "en") != 0;
    bool use_translation = false;
    if(translated) {
        if(!build_game_directory(
               bundled_directory,
               sizeof(bundled_directory),
               state->bundled_packs_dir,
               game_directory,
               state->lang) ||
           !build_game_directory(
               user_directory,
               sizeof(user_directory),
               state->user_packs_dir,
               game_directory,
               state->lang)) {
            heap_caps_free(candidates);
            return false;
        }
        use_translation = directory_has_pack(bundled_directory) || directory_has_pack(user_directory);
    }

    const char* language = use_translation ? state->lang : nullptr;
    if(!build_game_directory(
           bundled_directory,
           sizeof(bundled_directory),
           state->bundled_packs_dir,
           game_directory,
           language) ||
       !build_game_directory(
           user_directory,
           sizeof(user_directory),
           state->user_packs_dir,
           game_directory,
           language)) {
        heap_caps_free(candidates);
        return false;
    }

    size_t candidate_count = 0;
    bool ok = enumerate_pack_directory(candidates, &candidate_count, bundled_directory, false);
    ok = enumerate_pack_directory(candidates, &candidate_count, user_directory, true) && ok;
    sort_pack_candidates(candidates, candidate_count);

    const size_t limit = pack_limit(game_id);
    size_t loaded = 0;
    for(size_t index = 0; index < candidate_count && loaded < limit; index++) {
        /* Hand the CPU back between packs.
         *
         * A game change is approved inside the HTTP server callback, so this
         * whole loop runs on the httpd task -- reading pack after pack off the
         * SD card, on a SINGLE-CORE build, while holding engine_mutex. With
         * CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0 and a 10 s timeout, IDLE
         * never getting scheduled is a board reset: no exception, no
         * backtrace, just a reboot -- which is exactly the symptom.
         *
         * One tick per pack is nothing next to an SD read, and it is enough to
         * let IDLE feed the watchdog. */
        vTaskDelay(1);

        if(load_pack_file(*state->engine, game_id, candidates[index])) {
            loaded++;
        } else {
            ok = false;
            log_format(state, "Pack read failed: %s", candidates[index].name);
        }
    }
    heap_caps_free(candidates);

    if(candidate_count > limit) {
        log_format(state, "%s packs capped at %u", game_directory, static_cast<unsigned>(limit));
    }
    if(loaded == 0) {
        log_format(state, "No %s packs found", game_directory);
        return false;
    }
    log_format(
        state,
        "%s: %u pack%s%s",
        game_directory,
        static_cast<unsigned>(loaded),
        loaded == 1 ? "" : "s",
        use_translation ? " (translated)" : "");
    return ok;
}

MirrorPlayer* mirror_player(ServiceState* state, uint8_t pid) {
    if(!state || pid < 1 || pid > HA_MAX_PLAYERS) return nullptr;
    return &state->players[pid];
}

const char* mirror_nick(ServiceState* state, int pid) {
    MirrorPlayer* player = pid >= 0 ? mirror_player(state, static_cast<uint8_t>(pid)) : nullptr;
    return player && player->used && player->nick[0] ? player->nick : "?";
}

/* The artwork writer used stdio (fopen/fputs/fprintf/fclose). None of that
 * reaches the SD card on this port -- there is no VFS mount behind "/sdcard",
 * the storage service drives FatFs directly -- so these go through the Furi
 * storage API instead. Writes are best-effort: a failed write leaves the SVG
 * truncated rather than aborting the drawing round. */
void art_puts(ServiceState* state, const char* text) {
    if(!state || !state->art_file || !text) return;
    const size_t length = strlen(text);
    if(!length) return;
    if(storage_file_write(state->art_file, text, length) != length) {
        state->art_write_error = true;
    }
}

void art_printf(ServiceState* state, const char* format, ...) {
    if(!state || !state->art_file || !format) return;
    char line[512];
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if(written < 0) {
        state->art_write_error = true;
        return;
    }
    size_t length = static_cast<size_t>(written);
    /* vsnprintf() reports what it WOULD have written; a value past the buffer
     * means the line was truncated, so the SVG is already malformed. */
    if(length >= sizeof(line)) {
        length = sizeof(line) - 1;
        state->art_write_error = true;
    }
    if(length && storage_file_write(state->art_file, line, length) != length) {
        state->art_write_error = true;
    }
}

bool art_close(ServiceState* state) {
    if(!state || !state->art_file) return false;
    const bool closed = storage_file_close(state->art_file);
    storage_file_free(state->art_file);
    state->art_file = nullptr;
    return closed;
}

void art_abort(ServiceState* state) {
    if(!state) return;
    art_close(state);
    state->current_art_path[0] = '\0';
}

void xml_escape(const char* input, char* output, size_t output_size) {
    if(!output || !output_size) return;
    size_t used = 0;
    for(const unsigned char* cursor = reinterpret_cast<const unsigned char*>(input ? input : "");
        *cursor && used + 7 < output_size;
        cursor++) {
        const char* replacement = nullptr;
        switch(*cursor) {
        case '&':
            replacement = "&amp;";
            break;
        case '<':
            replacement = "&lt;";
            break;
        case '>':
            replacement = "&gt;";
            break;
        case '\"':
            replacement = "&quot;";
            break;
        case '\'':
            replacement = "&apos;";
            break;
        default:
            break;
        }
        if(replacement) {
            const size_t length = strlen(replacement);
            memcpy(output + used, replacement, length);
            used += length;
        } else if(*cursor >= 0x20) {
            output[used++] = static_cast<char>(*cursor);
        }
    }
    output[used] = '\0';
}

void art_begin(ServiceState* state, const char* json) {
    art_abort(state);
    int id = -1;
    if(!ha_json_int(json, "id", &id) || id < 0 || id > HA_MAX_PLAYERS) return;
    if(!ensure_directory(state->art_dir)) {
        log_line(state, "Artwork directory unavailable");
        return;
    }

    if(id == 0 || state->art_sequence == 0) state->art_sequence = esp_random();
    const int written = snprintf(
        state->current_art_path,
        sizeof(state->current_art_path),
        "%s/fd-%08lx-%d.svg",
        state->art_dir,
        static_cast<unsigned long>(state->art_sequence),
        id + 1);
    if(written < 0 || static_cast<size_t>(written) >= sizeof(state->current_art_path)) {
        state->current_art_path[0] = '\0';
        log_line(state, "Artwork path too long");
        return;
    }

    Storage* storage = static_cast<Storage*>(furi_record_open(RECORD_STORAGE));
    state->art_write_error = false;
    state->art_file = storage_file_alloc(storage);
    furi_record_close(RECORD_STORAGE);
    if(!storage_file_open(
           state->art_file, state->current_art_path, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(state->art_file);
        state->art_file = nullptr;
    }
    if(!state->art_file) {
        log_line(state, "Artwork open failed");
        state->current_art_path[0] = '\0';
        return;
    }

    art_puts(
        state,
        "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"510\" height=\"765\" "
        "viewBox=\"0 0 510 765\">\n"
        "<rect width=\"510\" height=\"765\" fill=\"#EDEDE6\"/>\n"
        "<path d=\"M0 255H510M0 510H510\" stroke=\"#C9C9BE\" stroke-width=\"2\"/>\n");

    char raw[HA_NICK_LEN];
    char names[3][HA_NICK_LEN * 6];
    for(int index = 0; index < 3; index++) {
        char key[3] = {'w', static_cast<char>('0' + index), '\0'};
        if(!ha_json_str(json, key, raw, sizeof(raw))) raw[0] = '\0';
        xml_escape(raw[0] ? raw : "-", names[index], sizeof(names[index]));
    }
    art_printf(
        state,
        "<text x=\"12\" y=\"752\" font-family=\"sans-serif\" font-size=\"18\" "
        "fill=\"#8A8A80\">%s / %s / %s</text>\n",
        names[0],
        names[1],
        names[2]);
    art_puts(
        state,
        "<g fill=\"none\" stroke=\"#111111\" stroke-width=\"5\" "
        "stroke-linecap=\"round\" stroke-linejoin=\"round\">\n");
}

void art_stroke(ServiceState* state, const char* json) {
    if(!state || !state->art_file) return;
    int x0;
    int y0;
    int x1;
    int y1;
    if(!ha_json_int(json, "x0", &x0) || !ha_json_int(json, "y0", &y0) ||
       !ha_json_int(json, "x1", &x1) || !ha_json_int(json, "y1", &y1))
        return;
    art_printf(state, "<path d=\"M%d %dL%d %d\"/>\n", x0 * 2, y0 * 3, x1 * 2, y1 * 3);
}

void art_end(ServiceState* state) {
    if(!state || !state->art_file) return;
    art_puts(state, "</g>\n</svg>\n");
    /* storage_file_close() flushes, so a clean close plus no short write along
     * the way is the equivalent of the old fflush()+fclose() pair. Order
     * matters: art_close() must run even when a write already failed. */
    const bool closed = art_close(state);
    const bool ok = closed && !state->art_write_error;

    const char* filename = strrchr(state->current_art_path, '/');
    filename = filename ? filename + 1 : state->current_art_path;
    if(ok)
        log_format(state, "Artwork saved: %s", filename);
    else
        log_line(state, "Artwork write failed");
    state->current_art_path[0] = '\0';
}

HotspotArcadeServiceResult map_runtime_result(HotspotArcadeRuntimeResult result) {
    switch(result) {
    case HotspotArcadeRuntimeResultOk:
        return HotspotArcadeServiceResultOk;
    case HotspotArcadeRuntimeResultInvalidArgument:
        return HotspotArcadeServiceResultInvalidArgument;
    case HotspotArcadeRuntimeResultAlreadyRunning:
        return HotspotArcadeServiceResultAlreadyRunning;
    case HotspotArcadeRuntimeResultNotRunning:
        return HotspotArcadeServiceResultNotRunning;
    case HotspotArcadeRuntimeResultNoMemory:
        return HotspotArcadeServiceResultNoMemory;
    case HotspotArcadeRuntimeResultStorageError:
        return HotspotArcadeServiceResultStorageError;
    case HotspotArcadeRuntimeResultWifiError:
    case HotspotArcadeRuntimeResultHttpError:
    case HotspotArcadeRuntimeResultDnsError:
    case HotspotArcadeRuntimeResultWebSocketDisabled:
        return HotspotArcadeServiceResultNetworkError;
    case HotspotArcadeRuntimeResultInternalError:
    default:
        return HotspotArcadeServiceResultInternalError;
    }
}

void destroy_state(ServiceState* state) {
    if(!state) return;
    art_abort(state);
    if(state->engine) {
        state->engine->reset();
        state->engine->~Engine();
        state->engine = nullptr;
    }
    if(state->engine_storage) {
        heap_caps_free(state->engine_storage);
        state->engine_storage = nullptr;
    }
    if(state->tick_done) {
        vSemaphoreDelete(state->tick_done);
        state->tick_done = nullptr;
    }
    if(state->command_done) {
        vSemaphoreDelete(state->command_done);
        state->command_done = nullptr;
    }
    if(state->engine_mutex) {
        vSemaphoreDelete(state->engine_mutex);
        state->engine_mutex = nullptr;
    }
    state->~ServiceState();
    heap_caps_free(state);
}

ServiceState* allocate_state() {
    void* storage = heap_caps_calloc(1, sizeof(ServiceState), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if(!storage) storage = heap_caps_calloc(1, sizeof(ServiceState), MALLOC_CAP_8BIT);
    if(!storage) return nullptr;
    ServiceState* state = new(storage) ServiceState{};

    state->engine_mutex = xSemaphoreCreateRecursiveMutex();
    state->tick_done = xSemaphoreCreateBinary();
    state->command_done = xSemaphoreCreateBinary();
    if(!state->engine_mutex || !state->tick_done || !state->command_done) {
        destroy_state(state);
        return nullptr;
    }

    /* The engine contains all game and bot state. Keep it PSRAM-only so adding
     * local players never consumes the internal DMA heap needed by the radios. */
    state->engine_storage =
        heap_caps_malloc(sizeof(Engine), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if(!state->engine_storage) {
        ESP_LOGE(kTag, "PSRAM allocation failed for %u-byte Engine", (unsigned)sizeof(Engine));
        destroy_state(state);
        return nullptr;
    }
    state->engine = new(state->engine_storage) Engine();
    return state;
}

void runtime_ws_open(void* context, HotspotArcadeRuntimeClientId client_id) {
    ServiceState* state = static_cast<ServiceState*>(context);
    if(!state || state != g_state.load(std::memory_order_acquire) ||
       !state->accepting.load(std::memory_order_acquire))
        return;
    EngineGuard guard(state);
    if(guard.locked()) log_format(state, "Socket %d opened", client_id);
}

void runtime_ws_close(void* context, HotspotArcadeRuntimeClientId client_id) {
    ServiceState* state = static_cast<ServiceState*>(context);
    if(!state || state != g_state.load(std::memory_order_acquire) || client_id <= 0 ||
       !state->accepting.load(std::memory_order_acquire))
        return;
    EngineGuard guard(state);
    if(guard.locked()) state->engine->onWsDisconnect(static_cast<uint32_t>(client_id));
}

void runtime_ws_text(
    void* context,
    HotspotArcadeRuntimeClientId client_id,
    const char* text,
    size_t length) {
    ServiceState* state = static_cast<ServiceState*>(context);
    if(!state || state != g_state.load(std::memory_order_acquire) || client_id <= 0 || !text ||
       length == 0 || length > kMaxWsMessageSize || memchr(text, '\0', length) ||
       !state->accepting.load(std::memory_order_acquire))
        return;

    char message[kMaxWsMessageSize + 1];
    memcpy(message, text, length);
    message[length] = '\0';
    EngineGuard guard(state);
    if(guard.locked()) {
        // The current browser sends its stable `cid` in hello. A zero transport
        // device key lets the engine use that identity without conflating clients.
        state->engine->onInput(static_cast<uint32_t>(client_id), 0, message);
    }
}

void tick_task(void* context) {
    ServiceState* state = static_cast<ServiceState*>(context);
    while(!state->tick_stop.load(std::memory_order_acquire)) {
        {
            EngineGuard guard(state);
            if(guard.locked() && state->accepting.load(std::memory_order_relaxed)) {
                /* Phone-approved switches are deliberately completed here on
                 * the dedicated 8 KB PSRAM-backed engine stack. The WebSocket
                 * callback only publishes phone_game and wakes this task. */
                const uint8_t phone_game =
                    state->phone_game.exchange(HA_GAME_NONE, std::memory_order_acq_rel);
                if(phone_game != HA_GAME_NONE) {
                    if(!load_content_for_game(state, phone_game)) {
                        /* Preserve the old behaviour: enter the requested game
                         * even when its optional pack failed, but make the
                         * problem visible in the console. This also dismisses
                         * the vote overlay instead of leaving phones stuck. */
                        log_line(state, "Phone-vote pack load failed");
                    }
                    state->engine->selectGame(phone_game);
                    state->active_game = phone_game;
                    for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
                        if(state->players[pid].used) state->players[pid].score = 0;
                    }
                    mirror_touch(state);
                    char event[kConsoleLineSize];
                    snprintf(event, sizeof(event), "Phones chose %s", game_name(phone_game));
                    set_last_event(state, event);
                }

                const EngineCommand command = static_cast<EngineCommand>(
                    state->command.exchange(EngineCommandNone, std::memory_order_acq_rel));
                if(command != EngineCommandNone) {
                    state->command_result = HotspotArcadeServiceResultOk;
                    if(command == EngineCommandSelectGame) {
                        const uint8_t game_id = state->command_game;
                        if(!load_content_for_game(state, game_id)) {
                            state->command_result = HotspotArcadeServiceResultStorageError;
                        } else {
                            state->engine->selectGame(game_id);
                            state->active_game = game_id;
                            for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
                                if(state->players[pid].used) state->players[pid].score = 0;
                            }
                            mirror_touch(state);
                            char event[kConsoleLineSize];
                            snprintf(event, sizeof(event), "Game changed: %s", game_name(game_id));
                            set_last_event(state, event);
                        }
                    } else if(command == EngineCommandResetScores) {
                        state->engine->resetScores();
                        for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
                            if(state->players[pid].used) state->players[pid].score = 0;
                        }
                        mirror_touch(state);
                        set_last_event(state, "Scores reset");
                    } else if(command == EngineCommandRoundEnd) {
                        state->engine->roundEnd();
                        set_last_event(state, "Round ended");
                    }
                    xSemaphoreGive(state->command_done);
                }
                state->engine->tick(millis());
                const uint8_t active = state->engine->activeGame();
                if(active != state->active_game) {
                    state->active_game = active;
                    mirror_touch(state);
                }
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kTickPeriodMs));
    }
    xSemaphoreGive(state->tick_done);
    /* Paired with xTaskCreateWithCaps() -- the plain vTaskDelete() would not
     * free a caps-allocated stack. */
    vTaskDeleteWithCaps(nullptr);
}

HotspotArcadeServiceResult run_engine_command(
    ServiceState* state,
    EngineCommand command,
    uint8_t game_id = HA_GAME_NONE) {
    if(!state || !state->tick_task || command == EngineCommandNone)
        return HotspotArcadeServiceResultInternalError;
    state->command_game = game_id;
    state->command_result = HotspotArcadeServiceResultInternalError;
    state->command.store(command, std::memory_order_release);
    xTaskNotifyGive(state->tick_task);
    if(xSemaphoreTake(state->command_done, portMAX_DELAY) != pdTRUE)
        return HotspotArcadeServiceResultInternalError;
    return state->command_result;
}

bool validate_config(const HotspotArcadeServiceConfig* config) {
    if(!config || !config->ssid || !config->web_gzip_path || !config->bundled_packs_dir ||
       !config->user_packs_dir)
        return false;
    const size_t ssid_length = strlen(config->ssid);
    if(ssid_length == 0 || ssid_length > 32 || !config->web_gzip_path[0] ||
       !config->bundled_packs_dir[0] || !config->user_packs_dir[0])
        return false;
    return !config->lang || strlen(config->lang) < 8;
}

void stop_tick_task(ServiceState* state) {
    if(!state || !state->tick_task) return;
    state->tick_stop.store(true, std::memory_order_release);
    xTaskNotifyGive(state->tick_task);
    xSemaphoreTake(state->tick_done, portMAX_DELAY);
    state->tick_task = nullptr;
}

} // namespace

/* Outside the anonymous namespace on purpose: start() unwinds this on its
 * failure paths, so it must be the SAME function stop() calls. */
static void restore_bluetooth(void) {
    if(!g_bt_disabled_by_us) return;
    Bt* bt = static_cast<Bt*>(furi_record_open(RECORD_BT));
    if(!bt) {
        ESP_LOGE(kTag, "Bluetooth record unavailable during restore");
        return;
    }
    /* Let WiFi/lwIP finish releasing controller buffers before BLE grabs
     * contiguous internal DMA again; starting in that window can crash. */
    furi_delay_ms(500);
    bt_start_stack(bt);
    furi_record_close(RECORD_BT);
    g_bt_disabled_by_us = false;
}

// Engine sinks. Every engine entry point is serialized by ServiceState::engine_mutex;
// the recursive form is required because the phone-vote event loads new content before
// Engine::selectGame() resumes and sends its first lobby frame.
void haWsSendWs(uint32_t ws_id, const String& message) {
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state || !ws_id || !state->runtime_started.load(std::memory_order_acquire)) return;
    hotspot_arcade_runtime_send_text(
        static_cast<HotspotArcadeRuntimeClientId>(ws_id), message.c_str(), message.length());
}

void haWsBroadcast(const String& message) {
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state || !state->runtime_started.load(std::memory_order_acquire)) return;
    hotspot_arcade_runtime_broadcast_text(message.c_str(), message.length());
}

void haUartJoin(uint8_t pid, const char* nick) {
    ServiceState* state = g_state.load(std::memory_order_acquire);
    MirrorPlayer* player = mirror_player(state, pid);
    if(!player) return;
    const bool is_new = !player->used;
    player->used = true;
    safe_copy(player->nick, nick, sizeof(player->nick));
    if(is_new) player->score = 0;
    log_format(state, "%s %s", is_new ? "JOIN" : "NAME", player->nick);
}

void haUartLeave(uint8_t pid) {
    ServiceState* state = g_state.load(std::memory_order_acquire);
    MirrorPlayer* player = mirror_player(state, pid);
    if(!player || !player->used) return;
    char nickname[HA_NICK_LEN];
    safe_copy(nickname, player->nick, sizeof(nickname));
    *player = MirrorPlayer{};
    log_format(state, "LEAVE %s", nickname);
}

void haUartScore(uint8_t pid, int delta, const char* reason) {
    (void)reason;
    ServiceState* state = g_state.load(std::memory_order_acquire);
    MirrorPlayer* player = mirror_player(state, pid);
    if(!player || !player->used) return;
    const int64_t updated = static_cast<int64_t>(player->score) + delta;
    player->score = updated > INT32_MAX ? INT32_MAX : updated < INT32_MIN ? INT32_MIN : updated;
    mirror_touch(state);
}

void haUartEvent(const String& json) {
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state) return;
    const char* message = json.c_str();
    char value[kConsoleLineSize];

    if(ha_json_str(message, "gamevote", value, sizeof(value)) &&
       strcmp(value, "approved") == 0) {
        int game_id = -1;
        if(ha_json_int(message, "id", &game_id) && game_id > HA_GAME_NONE &&
           game_id <= HA_GAME_TUGOFWAR) {
            /* onInput() calls this sink on the httpd/WebSocket task. Queue the
             * expensive SD reload + Engine::selectGame() for tick_task rather
             * than nesting it on that task's 6 KB internal stack. */
            state->phone_game.store(static_cast<uint8_t>(game_id), std::memory_order_release);
            if(state->tick_task) xTaskNotifyGive(state->tick_task);
        }
        return;
    }

    static const char* const keys[] = {
        "duel", "pong", "draw", "chess", "bs", "spyfall", "spectrum", "kmk", "secrets",
        "rps", "math", "simon", "impostor", "oddone", "dice", "bomb", "cats", "bid", "tug"};
    for(const char* key : keys) {
        if(ha_json_str(message, key, value, sizeof(value))) {
            set_last_event(state, value);
            return;
        }
    }
    if(ha_json_str(message, "chat", value, sizeof(value))) log_line(state, value);
}

void haUartRoundResult(const String& json) {
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state) return;
    const char* message = json.c_str();
    char event[kConsoleLineSize];
    char value[kConsoleLineSize];
    int winner = 0;
    int loser = 0;
    if(ha_json_int(message, "win", &winner)) {
        ha_json_int(message, "lose", &loser);
        snprintf(
            event,
            sizeof(event),
            "%s beat %s",
            mirror_nick(state, winner),
            mirror_nick(state, loser));
    } else {
        static const char* const keys[] = {
            "trivia",
            "draw",
            "scramble",
            "react",
            "wyr",
            "spectrum",
            "kmk",
            "secrets",
            "fillblank",
            "werewolf",
            "spyfall",
            "frankendraw",
            "rps",
            "math",
            "simon",
            "impostor",
            "bulls",
            "g2048",
            "snake",
            "mines",
            "memory",
            "p15",
            "hilo",
            "aim",
            "oddone",
            "dice",
            "bomb",
            "nim",
            "gomoku",
            "cats",
            "bid",
            "tug",
            "chess",
            "bs",
        };
        bool found = false;
        for(const char* key : keys) {
            if(ha_json_str(message, key, value, sizeof(value))) {
                safe_copy(event, value, sizeof(event));
                found = true;
                break;
            }
        }
        if(!found) {
            const char* draw = ha_json_find(message, "draw");
            safe_copy(event, draw && *draw == '[' ? "Round drawn" : message, sizeof(event));
        }
    }
    set_last_event(state, event);
}

void haLogJoin(uint8_t pid, uint64_t device_key, const char* nick, bool consolidated) {
    ESP_LOGI(
        kTag,
        "%s pid=%u key=%llu nick=%s",
        consolidated ? "recognized" : "identity",
        static_cast<unsigned>(pid),
        static_cast<unsigned long long>(device_key),
        nick ? nick : "");
}

void haUartArt(uint8_t operation, const String& json) {
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state) return;
    if(operation == HA_ART_BEGIN)
        art_begin(state, json.c_str());
    else if(operation == HA_ART_STROKE)
        art_stroke(state, json.c_str());
    else if(operation == HA_ART_END)
        art_end(state);
}

extern "C" {

HotspotArcadeServiceResult hotspot_arcade_service_start(
    const HotspotArcadeServiceConfig* config) {
    ApiGuard api_guard;
    if(!validate_config(config)) return HotspotArcadeServiceResultInvalidArgument;
    if(g_state.load(std::memory_order_acquire) || hotspot_arcade_runtime_is_running())
        return HotspotArcadeServiceResultAlreadyRunning;

    ServiceState* state = allocate_state();
    if(!state) {
        ESP_LOGE(kTag, "state alloc failed: internal DRAM free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        return HotspotArcadeServiceResultNoMemory;
    }
    if(!copy_checked(state->ssid, sizeof(state->ssid), config->ssid) ||
       !map_storage_path(
           config->bundled_packs_dir,
           state->bundled_packs_dir,
           sizeof(state->bundled_packs_dir)) ||
       !map_storage_path(
           config->user_packs_dir, state->user_packs_dir, sizeof(state->user_packs_dir)) ||
       !derive_art_directory(state->user_packs_dir, state->art_dir, sizeof(state->art_dir)) ||
       !copy_checked(state->lang, sizeof(state->lang), config->lang ? config->lang : "")) {
        destroy_state(state);
        return HotspotArcadeServiceResultInvalidArgument;
    }

    {
        /* Report WHICH path failed. All three collapse into one opaque
         * "storage error" otherwise, which is impossible to act on from the
         * device screen alone. */
        const bool packs_ok = path_is_directory(state->bundled_packs_dir);
        const bool user_ok = ensure_directory(state->user_packs_dir);
        const bool art_ok = ensure_directory(state->art_dir);
        if(!packs_ok || !user_ok || !art_ok) {
            ESP_LOGE(
                kTag,
                "storage check failed: bundled_packs(%s)=%d user_packs(%s)=%d art(%s)=%d",
                state->bundled_packs_dir,
                packs_ok,
                state->user_packs_dir,
                user_ok,
                state->art_dir,
                art_ok);
            destroy_state(state);
            return HotspotArcadeServiceResultStorageError;
        }
    }

    {
        EngineGuard guard(state);
        if(!guard.locked()) {
            destroy_state(state);
            return HotspotArcadeServiceResultInternalError;
        }
        state->engine->reset();
        state->engine->contentClear();
        state->engine->setLang(strcmp(state->lang, "en") == 0 ? "" : state->lang);
        state->active_game = HA_GAME_NONE;
        log_line(state, "Hotspot Arcade starting");
        /* Internal DRAM is the scarce one: PSRAM is plentiful here, but task
         * stacks and the WiFi/netif core can only come from internal. The
         * aggregate "free heap" figure hides that entirely. */
        ESP_LOGI(
            kTag,
            "start: internal DRAM free=%u largest=%u, total free=%u",
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
            (unsigned)esp_get_free_heap_size());
    }

    g_state.store(state, std::memory_order_release);
    state->accepting.store(true, std::memory_order_release);
    HotspotArcadeRuntimeConfig runtime_config{};
    runtime_config.ssid = state->ssid;
    runtime_config.web_gzip_path = config->web_gzip_path;
    runtime_config.callback_context = state;
    runtime_config.on_ws_open = runtime_ws_open;
    runtime_config.on_ws_close = runtime_ws_close;
    runtime_config.on_ws_text = runtime_ws_text;
    runtime_config.max_ws_message_size = kMaxWsMessageSize;
    runtime_config.channel = HOTSPOT_ARCADE_RUNTIME_DEFAULT_CHANNEL;
    runtime_config.max_connections = HOTSPOT_ARCADE_RUNTIME_DEFAULT_MAX_CONNECTIONS;

    ESP_LOGI(
        kTag,
        "starting Hotspot Arcade alongside BLE: internal free=%u largest=%u | DMA free=%u largest=%u",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));

    /* Free the BLE controller before softAP comes up. Both radios share the
     * chip and both want INTERNAL DRAM; the controller holds ~60 KB of it and
     * WiFi's esf_buf/softAP pools need CONTIGUOUS DMA-capable internal memory
     * that PSRAM cannot back. NimBLE coexistence leaves too little (measured:
     * DMA free ~1.7 KB), so softAP attach crashed. Stop the running stack ONLY;
     * do not persist the setting, so a reset/crash comes back with BLE on and
     * restore_bluetooth() puts it back for a clean exit. */
    {
        Bt* bt = static_cast<Bt*>(furi_record_open(RECORD_BT));
        if(bt && bt_is_enabled(bt)) {
            bt_stop_stack(bt);
            g_bt_disabled_by_us = true;
            ESP_LOGI(kTag, "BLE stack stopped to free internal DRAM for WiFi");
            furi_delay_ms(300);
            ESP_LOGI(
                kTag,
                "after BT off: internal free=%u largest=%u | DMA free=%u largest=%u",
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        }
        if(bt) furi_record_close(RECORD_BT);
    }

    const HotspotArcadeRuntimeResult runtime_result =
        hotspot_arcade_runtime_start(&runtime_config);
    if(runtime_result != HotspotArcadeRuntimeResultOk) {
        ESP_LOGE(
            kTag,
            "runtime_start failed: rc=%d, internal DRAM free=%u largest=%u",
            (int)runtime_result,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        state->accepting.store(false, std::memory_order_release);
        g_state.store(nullptr, std::memory_order_release);
        destroy_state(state);
        restore_bluetooth();
        return map_runtime_result(runtime_result);
    }
    state->runtime_started.store(true, std::memory_order_release);

    /* Engine stack goes in PSRAM, not internal DRAM.
     *
     * This was the last thing standing: with the AP already up, internal DRAM
     * was down to ~10 KB free / 7 KB largest, and an 8 KB contiguous internal
     * stack simply was not there. PSRAM has ~5 MB free and the engine is
     * ordinary task code -- no ISR context -- so an external stack is fine.
     * CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY and
     * CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM are already enabled.
     *
     * Must be torn down with vTaskDeleteWithCaps(), which tick_task does. */
    if(xTaskCreateWithCaps(
           tick_task,
           "ha_engine",
           kTickTaskStackSize,
           state,
           kTickTaskPriority,
           &state->tick_task,
           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE(
            kTag,
            "ha_engine task create failed (%u byte stack): internal DRAM free=%u largest=%u",
            (unsigned)kTickTaskStackSize,
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
            (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
        state->accepting.store(false, std::memory_order_release);
        state->runtime_started.store(false, std::memory_order_release);
        hotspot_arcade_runtime_stop();
        g_state.store(nullptr, std::memory_order_release);
        destroy_state(state);
        restore_bluetooth();
        return HotspotArcadeServiceResultNoMemory;
    }

    /* Hold the screen and the CPU awake for the whole session.
     *
     * Not optional here: players are connected over WiFi and the host has no
     * input events of its own, so the idle dim would black the screen and deep
     * sleep would drop the SoftAP out from under them mid-game. Released again
     * in hotspot_arcade_service_stop().
     *
     * Guarded by a flag because insomnia is a counter -- a second start without
     * an intervening stop must not enter it twice. */
    if(!g_wake_held) {
        g_wake_held = true;
        furi_hal_power_insomnia_enter();
        NotificationApp* notifications =
            static_cast<NotificationApp*>(furi_record_open(RECORD_NOTIFICATION));
        notification_message(notifications, &sequence_display_backlight_enforce_on);
        furi_record_close(RECORD_NOTIFICATION);
    }

    {
        EngineGuard guard(state);
        if(guard.locked()) set_last_event(state, "Hotspot Arcade ready");
    }
    return HotspotArcadeServiceResultOk;
}

static void release_wake_lock(void) {
    if(!g_wake_held) return;
    g_wake_held = false;
    NotificationApp* notifications =
        static_cast<NotificationApp*>(furi_record_open(RECORD_NOTIFICATION));
    notification_message(notifications, &sequence_display_backlight_enforce_auto);
    furi_record_close(RECORD_NOTIFICATION);
    furi_hal_power_insomnia_exit();
}

void hotspot_arcade_service_stop(void) {
    ApiGuard api_guard;
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state) return;

    release_wake_lock();

    state->accepting.store(false, std::memory_order_release);
    stop_tick_task(state);
    if(state->runtime_started.exchange(false, std::memory_order_acq_rel)) {
        const HotspotArcadeRuntimeResult result = hotspot_arcade_runtime_stop();
        if(result != HotspotArcadeRuntimeResultOk &&
           result != HotspotArcadeRuntimeResultNotRunning) {
            ESP_LOGW(kTag, "runtime stop: %s", hotspot_arcade_runtime_result_to_string(result));
        }
    }

    g_state.store(nullptr, std::memory_order_release);
    {
        EngineGuard guard(state);
        if(guard.locked()) {
            art_abort(state);
            state->engine->reset();
        }
    }
    destroy_state(state);
    restore_bluetooth();
}

bool hotspot_arcade_service_is_running(void) {
    ApiGuard api_guard;
    ServiceState* state = g_state.load(std::memory_order_acquire);
    return state && state->accepting.load(std::memory_order_acquire) &&
           state->runtime_started.load(std::memory_order_acquire) &&
           hotspot_arcade_runtime_is_running();
}

HotspotArcadeServiceResult hotspot_arcade_service_select_game(uint8_t game_id) {
    ApiGuard api_guard;
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state || !state->accepting.load(std::memory_order_acquire))
        return HotspotArcadeServiceResultNotRunning;
    if(game_id > HA_GAME_TUGOFWAR) return HotspotArcadeServiceResultInvalidArgument;
    return run_engine_command(state, EngineCommandSelectGame, game_id);
}

void hotspot_arcade_service_reset_scores(void) {
    ApiGuard api_guard;
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state || !state->accepting.load(std::memory_order_acquire)) return;
    run_engine_command(state, EngineCommandResetScores);
}

void hotspot_arcade_service_round_end(void) {
    ApiGuard api_guard;
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state || !state->accepting.load(std::memory_order_acquire)) return;
    run_engine_command(state, EngineCommandRoundEnd);
}

bool hotspot_arcade_service_snapshot(HotspotArcadeServiceSnapshot* out_snapshot) {
    if(!out_snapshot) return false;
    memset(out_snapshot, 0, sizeof(*out_snapshot));
    ApiGuard api_guard;
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state) return false;
    EngineGuard guard(state);
    if(!guard.locked()) return false;

    const uint8_t engine_game = state->engine->activeGame();
    if(engine_game != state->active_game) {
        state->active_game = engine_game;
        mirror_touch(state);
    }
    out_snapshot->revision = state->revision;
    out_snapshot->running = state->accepting.load(std::memory_order_acquire) &&
                            state->runtime_started.load(std::memory_order_acquire) &&
                            hotspot_arcade_runtime_is_running();
    out_snapshot->active_game = state->active_game;
    const size_t connected = hotspot_arcade_runtime_connected_count();
    out_snapshot->client_count =
        static_cast<uint8_t>(connected > UINT8_MAX ? UINT8_MAX : connected);
    safe_copy(out_snapshot->ssid, state->ssid, sizeof(out_snapshot->ssid));
    safe_copy(out_snapshot->ip, out_snapshot->running ? kApIp : "", sizeof(out_snapshot->ip));
    safe_copy(out_snapshot->last_event, state->last_event, sizeof(out_snapshot->last_event));

    for(uint8_t pid = 1; pid <= HA_MAX_PLAYERS; pid++) {
        const MirrorPlayer& source = state->players[pid];
        HotspotArcadeServicePlayer& destination = out_snapshot->players[pid - 1];
        destination.used = source.used;
        destination.pid = pid;
        destination.score = source.score;
        safe_copy(destination.nick, source.nick, sizeof(destination.nick));
        if(source.used) out_snapshot->player_count++;
    }
    return true;
}

size_t hotspot_arcade_service_copy_console(char* buffer, size_t buffer_size) {
    if(buffer && buffer_size) buffer[0] = '\0';
    ApiGuard api_guard;
    ServiceState* state = g_state.load(std::memory_order_acquire);
    if(!state || !buffer || buffer_size == 0) return 0;
    EngineGuard guard(state);
    if(!guard.locked()) return 0;

    const uint32_t count = state->console_total < kConsoleLines ? state->console_total : kConsoleLines;
    const uint32_t first = state->console_total - count;
    size_t used = 0;
    for(uint32_t index = 0; index < count && used + 1 < buffer_size; index++) {
        const char* line = state->console[(first + index) % kConsoleLines];
        const size_t line_length = strlen(line);
        const size_t available = buffer_size - used - 1;
        const size_t copied = line_length < available ? line_length : available;
        if(copied) memcpy(buffer + used, line, copied);
        used += copied;
        if(copied != line_length) break;
        if(index + 1 < count && used + 1 < buffer_size) buffer[used++] = '\n';
    }
    buffer[used] = '\0';
    return used;
}

const char* hotspot_arcade_service_result_to_string(HotspotArcadeServiceResult result) {
    switch(result) {
    case HotspotArcadeServiceResultOk:
        return "ok";
    case HotspotArcadeServiceResultInvalidArgument:
        return "invalid argument";
    case HotspotArcadeServiceResultAlreadyRunning:
        return "already running";
    case HotspotArcadeServiceResultNotRunning:
        return "not running";
    case HotspotArcadeServiceResultStorageError:
        return "storage error";
    case HotspotArcadeServiceResultNetworkError:
        return "network error";
    case HotspotArcadeServiceResultNoMemory:
        return "out of memory";
    case HotspotArcadeServiceResultInternalError:
        return "internal error";
    default:
        return "unknown";
    }
}

} // extern "C"
