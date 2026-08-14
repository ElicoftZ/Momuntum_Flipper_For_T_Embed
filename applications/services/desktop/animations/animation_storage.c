
#include <stdint.h>
#include <flipper_format/flipper_format.h>
#include <furi.h>
#include <core/dangerous_defines.h>
#include <storage/storage.h>
#include <gui/icon_i.h>
#include <toolbox/compress.h>

#include "animation_manager.h"
#include "animation_storage.h"
#include <assets_dolphin_internal.h>
#include <assets_dolphin_blocking.h>
#include <momentum/momentum.h>

#define ANIMATION_META_FILE     "meta.txt"
#define ANIMATION_MANIFEST_FILE "manifest.txt"
#define TAG                     "AnimationStorage"

static void animation_storage_free_bubbles(BubbleAnimation* animation);
static void animation_storage_free_frames(BubbleAnimation* animation);
static void animation_storage_free_animation(BubbleAnimation** storage_animation);
static BubbleAnimation* animation_storage_load_animation(const char* name);

static void animation_storage_resolve_directory(Storage* storage, FuriString* directory) {
    furi_assert(storage);
    furi_assert(directory);

    furi_string_set(directory, BASE_ANIMATION_DIR);

    const char* asset_pack = momentum_settings.asset_pack;
    if(!asset_pack[0]) return;

    if(asset_pack[ASSET_PACKS_NAME_LEN - 1] ||
       !momentum_settings_asset_pack_is_safe(asset_pack)) {
        FURI_LOG_W(TAG, "Ignoring unsafe asset pack name");
        return;
    }

    FuriString* candidate = furi_string_alloc_printf(
        "%s/%s/Anims", ASSET_PACKS_PATH, asset_pack);
    FuriString* manifest = furi_string_alloc_printf(
        "%s/%s", furi_string_get_cstr(candidate), ANIMATION_MANIFEST_FILE);

    if(storage_common_stat(storage, furi_string_get_cstr(manifest), NULL) == FSE_OK) {
        furi_string_set(directory, candidate);
    } else {
        FURI_LOG_W(TAG, "Asset pack animation manifest missing, using base animations");
    }

    furi_string_free(manifest);
    furi_string_free(candidate);
}

static void animation_storage_resolve_manifest(
    Storage* storage,
    FuriString* directory,
    FuriString* manifest) {
    animation_storage_resolve_directory(storage, directory);
    furi_string_printf(
        manifest,
        "%s/%s",
        furi_string_get_cstr(directory),
        ANIMATION_MANIFEST_FILE);
}

static bool animation_storage_load_single_manifest_info(
    StorageAnimationManifestInfo* manifest_info,
    const char* name) {
    furi_assert(manifest_info);
    furi_assert(name);

    memset(manifest_info, 0, sizeof(*manifest_info));

    bool result = false;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    flipper_format_set_strict_mode(file, true);
    FuriString* read_string = furi_string_alloc();
    FuriString* directory = furi_string_alloc();
    FuriString* manifest = furi_string_alloc();
    animation_storage_resolve_manifest(storage, directory, manifest);

    do {
        uint32_t u32value;
        if(FSE_OK != storage_sd_status(storage)) break;
        if(!flipper_format_file_open_existing(file, furi_string_get_cstr(manifest))) break;

        if(!flipper_format_read_header(file, read_string, &u32value)) break;
        if(furi_string_cmp_str(read_string, "Flipper Animation Manifest")) break;

        /* skip other animation names */
        flipper_format_set_strict_mode(file, false);
        while(flipper_format_read_string(file, "Name", read_string) &&
              furi_string_cmp_str(read_string, name))
            ;
        if(furi_string_cmp_str(read_string, name)) break;
        flipper_format_set_strict_mode(file, true);

        manifest_info->name = strdup(furi_string_get_cstr(read_string));

        if(!flipper_format_read_uint32(file, "Min butthurt", &u32value, 1)) break;
        manifest_info->min_butthurt = u32value;
        if(!flipper_format_read_uint32(file, "Max butthurt", &u32value, 1)) break;
        manifest_info->max_butthurt = u32value;
        if(!flipper_format_read_uint32(file, "Min level", &u32value, 1)) break;
        manifest_info->min_level = u32value;
        if(!flipper_format_read_uint32(file, "Max level", &u32value, 1)) break;
        manifest_info->max_level = u32value;
        if(!flipper_format_read_uint32(file, "Weight", &u32value, 1)) break;
        manifest_info->weight = u32value;
        result = true;
    } while(0);

    if(!result && manifest_info->name) {
        free((void*)manifest_info->name);
        manifest_info->name = NULL;
    }
    furi_string_free(manifest);
    furi_string_free(directory);
    furi_string_free(read_string);
    flipper_format_free(file);

    furi_record_close(RECORD_STORAGE);

    return result;
}

void animation_storage_fill_animation_list(StorageAnimationList_t* animation_list) {
    furi_assert(sizeof(StorageAnimationList_t) == sizeof(void*));
    furi_assert(!StorageAnimationList_size(*animation_list));

    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* file = flipper_format_file_alloc(storage);
    /* Forbid skipping fields */
    flipper_format_set_strict_mode(file, true);
    FuriString* read_string = furi_string_alloc();
    FuriString* directory = furi_string_alloc();
    FuriString* manifest = furi_string_alloc();
    animation_storage_resolve_manifest(storage, directory, manifest);

    do {
        uint32_t u32value;
        StorageAnimation* storage_animation = NULL;

        if(FSE_OK != storage_sd_status(storage)) break;
        if(!flipper_format_file_open_existing(file, furi_string_get_cstr(manifest))) break;
        if(!flipper_format_read_header(file, read_string, &u32value)) break;
        if(furi_string_cmp_str(read_string, "Flipper Animation Manifest")) break;
        do {
            storage_animation = malloc(sizeof(StorageAnimation));
            memset(storage_animation, 0, sizeof(*storage_animation));
            storage_animation->external = true;

            if(!flipper_format_read_string(file, "Name", read_string)) break;
            storage_animation->manifest_info.name = strdup(furi_string_get_cstr(read_string));

            if(!flipper_format_read_uint32(file, "Min butthurt", &u32value, 1)) break;
            storage_animation->manifest_info.min_butthurt = u32value;
            if(!flipper_format_read_uint32(file, "Max butthurt", &u32value, 1)) break;
            storage_animation->manifest_info.max_butthurt = u32value;
            if(!flipper_format_read_uint32(file, "Min level", &u32value, 1)) break;
            storage_animation->manifest_info.min_level = u32value;
            if(!flipper_format_read_uint32(file, "Max level", &u32value, 1)) break;
            storage_animation->manifest_info.max_level = u32value;
            if(!flipper_format_read_uint32(file, "Weight", &u32value, 1)) break;
            storage_animation->manifest_info.weight = u32value;

            StorageAnimationList_push_back(*animation_list, storage_animation);
        } while(1);

        animation_storage_free_storage_animation(&storage_animation);
    } while(0);

    furi_string_free(manifest);
    furi_string_free(directory);
    furi_string_free(read_string);
    flipper_format_free(file);

    // add hard-coded animations
    for(size_t i = 0; i < dolphin_internal_size; ++i) {
        StorageAnimationList_push_back(*animation_list, (StorageAnimation*)&dolphin_internal[i]);
    }

    furi_record_close(RECORD_STORAGE);
}

StorageAnimation* animation_storage_find_animation(const char* name) {
    furi_assert(name);
    furi_assert(strlen(name));
    StorageAnimation* storage_animation = NULL;

    for(size_t i = 0; i < dolphin_blocking_size; ++i) {
        if(!strcmp(dolphin_blocking[i].manifest_info.name, name)) {
            storage_animation = (StorageAnimation*)&dolphin_blocking[i];
            break;
        }
    }

    if(!storage_animation) {
        for(size_t i = 0; i < dolphin_internal_size; ++i) {
            if(!strcmp(dolphin_internal[i].manifest_info.name, name)) {
                storage_animation = (StorageAnimation*)&dolphin_internal[i];
                break;
            }
        }
    }

    /* look through external animations */
    if(!storage_animation) {
        storage_animation = malloc(sizeof(StorageAnimation));
        memset(storage_animation, 0, sizeof(*storage_animation));
        storage_animation->external = true;

        bool result = false;
        result =
            animation_storage_load_single_manifest_info(&storage_animation->manifest_info, name);
        if(result) {
            storage_animation->animation = animation_storage_load_animation(name);
            result = !!storage_animation->animation;
        }
        if(!result) {
            animation_storage_free_storage_animation(&storage_animation);
        }
    }

    return storage_animation;
}

StorageAnimationManifestInfo* animation_storage_get_meta(StorageAnimation* storage_animation) {
    furi_assert(storage_animation);
    return &storage_animation->manifest_info;
}

const BubbleAnimation*
    animation_storage_get_bubble_animation(StorageAnimation* storage_animation) {
    furi_assert(storage_animation);
    animation_storage_cache_animation(storage_animation);
    return storage_animation->animation;
}

void animation_storage_cache_animation(StorageAnimation* storage_animation) {
    furi_assert(storage_animation);

    if(storage_animation->external) {
        if(!storage_animation->animation) {
            storage_animation->animation =
                animation_storage_load_animation(storage_animation->manifest_info.name);
        }
    }
}

static void animation_storage_free_animation(BubbleAnimation** animation) {
    furi_assert(animation);

    if(*animation) {
        animation_storage_free_bubbles(*animation);
        animation_storage_free_frames(*animation);
        if((*animation)->frame_order) {
            free((void*)(*animation)->frame_order);
        }
        free(*animation);
        *animation = NULL;
    }
}

void animation_storage_free_storage_animation(StorageAnimation** storage_animation) {
    furi_assert(storage_animation);
    furi_assert(*storage_animation);

    if((*storage_animation)->external) {
        animation_storage_free_animation((BubbleAnimation**)&(*storage_animation)->animation);

        if((*storage_animation)->manifest_info.name) {
            free((void*)(*storage_animation)->manifest_info.name);
        }
        free(*storage_animation);
    }

    *storage_animation = NULL;
}

static bool animation_storage_cast_align(FuriString* align_str, Align* align) {
    if(!furi_string_cmp(align_str, "Bottom")) {
        *align = AlignBottom;
    } else if(!furi_string_cmp(align_str, "Top")) {
        *align = AlignTop;
    } else if(!furi_string_cmp(align_str, "Left")) {
        *align = AlignLeft;
    } else if(!furi_string_cmp(align_str, "Right")) {
        *align = AlignRight;
    } else if(!furi_string_cmp(align_str, "Center")) {
        *align = AlignCenter;
    } else {
        return false;
    }

    return true;
}

static void animation_storage_free_frames(BubbleAnimation* animation) {
    furi_assert(animation);

    const Icon* icon = &animation->icon_animation;
    if(!icon->frames) return;

    for(int i = 0; i < icon->frame_count; ++i) {
        if(icon->frames[i]) {
            free((void*)icon->frames[i]);
        }
    }

    free((void*)icon->frames);
    FURI_CONST_ASSIGN_PTR(icon->frames, NULL);
}

static bool animation_storage_load_frames(
    Storage* storage,
    const char* directory,
    const char* name,
    BubbleAnimation* animation,
    uint32_t* frame_order,
    uint8_t width,
    uint8_t height) {
    uint16_t frame_order_count = animation->passive_frames + animation->active_frames;

    /* The frames should go in order (0...N), without omissions */
    size_t max_frame_count = 0;
    for(int i = 0; i < frame_order_count; ++i) {
        max_frame_count = MAX(max_frame_count, frame_order[i]);
    }

    if((max_frame_count >= frame_order_count) || (max_frame_count >= 256 /* max uint8_t */)) {
        return false;
    }

    Icon* icon = (Icon*)&animation->icon_animation;
    FURI_CONST_ASSIGN(icon->frame_count, max_frame_count + 1);
    FURI_CONST_ASSIGN(icon->frame_rate, 0);
    FURI_CONST_ASSIGN(icon->height, height);
    FURI_CONST_ASSIGN(icon->width, width);
    icon->frames = malloc(sizeof(const uint8_t*) * icon->frame_count);
    memset((void*)icon->frames, 0, sizeof(const uint8_t*) * icon->frame_count);

    bool frames_ok = false;
    File* file = storage_file_alloc(storage);
    FileInfo file_info = {0};
    FuriString* filename;
    filename = furi_string_alloc();
    size_t max_filesize = ROUND_UP_TO(width, 8) * height + 1;
    Compress* frame_decoder = compress_alloc(
        CompressTypeHeatshrink, &compress_config_heatshrink_default);
    uint8_t* decoded_frame = malloc(max_filesize - 1U);

    for(int i = 0; i < icon->frame_count; ++i) {
        frames_ok = false;
        furi_string_printf(filename, "%s/%s/frame_%d.bm", directory, name, i);

        if(storage_common_stat(storage, furi_string_get_cstr(filename), &file_info) != FSE_OK)
            break;
        if((file_info.size == 0) || (file_info.size > max_filesize)) {
            FURI_LOG_E(
                TAG,
                "Filesize %llu, max: %zu (width %u, height %u)",
                file_info.size,
                max_filesize,
                width,
                height);
            break;
        }
        if(!storage_file_open(
               file, furi_string_get_cstr(filename), FSAM_READ, FSOM_OPEN_EXISTING)) {
            FURI_LOG_E(TAG, "Can't open file \'%s\'", furi_string_get_cstr(filename));
            break;
        }

        /* Freeze-frame cloning copies the full uncompressed size. Keep every
         * external frame zero-padded to that size even when the file is compressed. */
        FURI_CONST_ASSIGN_PTR(icon->frames[i], calloc(1, max_filesize));
        if(storage_file_read(file, (void*)icon->frames[i], file_info.size) != file_info.size) {
            FURI_LOG_E(TAG, "Read failed: \'%s\'", furi_string_get_cstr(filename));
            break;
        }
        storage_file_close(file);

        const uint8_t* frame = icon->frames[i];
        bool frame_valid = false;
        if(frame[0] == 0) {
            frame_valid = file_info.size == max_filesize;
        } else if(file_info.size >= 4) {
            const uint16_t encoded_size = (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
            size_t decoded_size = 0;
            frame_valid =
                encoded_size && ((size_t)encoded_size + 4U == file_info.size) &&
                compress_decode(
                    frame_decoder,
                    (uint8_t*)frame,
                    file_info.size,
                    decoded_frame,
                    max_filesize - 1U,
                    &decoded_size) &&
                (decoded_size == max_filesize - 1U);
        }
        if(!frame_valid) {
            FURI_LOG_E(TAG, "Invalid bitmap frame: \'%s\'", furi_string_get_cstr(filename));
            break;
        }

        frames_ok = true;
    }

    if(!frames_ok) {
        FURI_LOG_E(
            TAG,
            "Load \'%s\' failed, %ux%u, size: %llu",
            furi_string_get_cstr(filename),
            width,
            height,
            file_info.size);
        animation_storage_free_frames(animation);
    } else {
        furi_check(animation->icon_animation.frames);
        for(int i = 0; i < animation->icon_animation.frame_count; ++i) {
            furi_check(animation->icon_animation.frames[i]);
        }
    }

    storage_file_free(file);
    furi_string_free(filename);
    free(decoded_frame);
    compress_free(frame_decoder);

    return frames_ok;
}

static bool animation_storage_load_bubbles(BubbleAnimation* animation, FlipperFormat* ff) {
    uint32_t u32value;
    FuriString* str;
    str = furi_string_alloc();
    bool success = false;
    furi_assert(!animation->frame_bubble_sequences);

    do {
        if(!flipper_format_read_uint32(ff, "Bubble slots", &u32value, 1)) break;
        if(u32value > 20) break;
        animation->frame_bubble_sequences_count = u32value;
        if(animation->frame_bubble_sequences_count == 0) {
            success = true;
            break;
        }
        animation->frame_bubble_sequences =
            calloc(animation->frame_bubble_sequences_count, sizeof(FrameBubble*));

        int32_t current_slot = 0;
        for(int i = 0; i < animation->frame_bubble_sequences_count; ++i) {
            FURI_CONST_ASSIGN_PTR(
                animation->frame_bubble_sequences[i], calloc(1, sizeof(FrameBubble)));
        }

        const FrameBubble* bubble = animation->frame_bubble_sequences[0];
        int8_t index = -1;
        bool parse_error = false;
        for(;;) {
            if(!flipper_format_read_int32(ff, "Slot", &current_slot, 1)) break;
            if((current_slot < 0) ||
               (current_slot >= (int32_t)animation->frame_bubble_sequences_count) ||
               ((current_slot != 0) && (index == -1))) {
                parse_error = true;
                break;
            }

            if(current_slot == index) {
                FURI_CONST_ASSIGN_PTR(bubble->next_bubble, calloc(1, sizeof(FrameBubble)));
                bubble = bubble->next_bubble;
            } else if(current_slot == index + 1) {
                ++index;
                bubble = animation->frame_bubble_sequences[index];
            } else {
                /* slots have to start from 0, be ascending sorted, and
                 * have exact number of slots as specified in "Bubble slots" */
                parse_error = true;
                break;
            }

            if(!flipper_format_read_uint32(ff, "X", &u32value, 1) ||
               (u32value > UINT8_MAX)) {
                parse_error = true;
                break;
            }
            FURI_CONST_ASSIGN(bubble->bubble.x, u32value);
            if(!flipper_format_read_uint32(ff, "Y", &u32value, 1) ||
               (u32value > UINT8_MAX)) {
                parse_error = true;
                break;
            }
            FURI_CONST_ASSIGN(bubble->bubble.y, u32value);

            if(!flipper_format_read_string(ff, "Text", str) ||
               (furi_string_size(str) > 100)) {
                parse_error = true;
                break;
            }

            furi_string_replace_all(str, "\\n", "\n");

            FURI_CONST_ASSIGN_PTR(bubble->bubble.text, strdup(furi_string_get_cstr(str)));

            if(!flipper_format_read_string(ff, "AlignH", str) ||
               !animation_storage_cast_align(str, (Align*)&bubble->bubble.align_h) ||
               !flipper_format_read_string(ff, "AlignV", str) ||
               !animation_storage_cast_align(str, (Align*)&bubble->bubble.align_v)) {
                parse_error = true;
                break;
            }

            if(!flipper_format_read_uint32(ff, "StartFrame", &u32value, 1) ||
               (u32value > UINT8_MAX)) {
                parse_error = true;
                break;
            }
            FURI_CONST_ASSIGN(bubble->start_frame, u32value);
            if(!flipper_format_read_uint32(ff, "EndFrame", &u32value, 1) ||
               (u32value > UINT8_MAX) || (u32value < bubble->start_frame)) {
                parse_error = true;
                break;
            }
            FURI_CONST_ASSIGN(bubble->end_frame, u32value);
        }
        success = !parse_error && ((index + 1) == animation->frame_bubble_sequences_count);
    } while(0);

    if(!success) {
        if(animation->frame_bubble_sequences) {
            FURI_LOG_E(TAG, "Failed to load animation bubbles");
            animation_storage_free_bubbles(animation);
        }
    }

    furi_string_free(str);
    return success;
}

static BubbleAnimation* animation_storage_load_animation(const char* name) {
    furi_assert(name);
    BubbleAnimation* animation = malloc(sizeof(BubbleAnimation));
    memset(animation, 0, sizeof(*animation));

    uint32_t height = 0;
    uint32_t width = 0;
    uint32_t* u32array = NULL;
    Storage* storage = furi_record_open(RECORD_STORAGE);
    FlipperFormat* ff = flipper_format_file_alloc(storage);
    /* Forbid skipping fields */
    flipper_format_set_strict_mode(ff, true);
    FuriString* str = furi_string_alloc();
    FuriString* directory = furi_string_alloc();
    animation_storage_resolve_directory(storage, directory);

    bool success = false;
    do {
        uint32_t u32value;

        if(FSE_OK != storage_sd_status(storage)) break;

        furi_string_printf(
            str,
            "%s/%s/%s",
            furi_string_get_cstr(directory),
            name,
            ANIMATION_META_FILE);
        if(!flipper_format_file_open_existing(ff, furi_string_get_cstr(str))) break;
        if(!flipper_format_read_header(ff, str, &u32value)) break;
        if(furi_string_cmp_str(str, "Flipper Animation")) break;

        if(!flipper_format_read_uint32(ff, "Width", &width, 1)) break;
        if(!flipper_format_read_uint32(ff, "Height", &height, 1)) break;
        if((width == 0) || (width > UINT8_MAX) || (height == 0) || (height > UINT8_MAX)) {
            FURI_LOG_E(
                TAG,
                "Invalid animation dimensions: %lux%lu",
                (unsigned long)width,
                (unsigned long)height);
            break;
        }

        uint32_t passive_frames;
        uint32_t active_frames;
        if(!flipper_format_read_uint32(ff, "Passive frames", &u32value, 1)) break;
        passive_frames = u32value;
        if(!flipper_format_read_uint32(ff, "Active frames", &u32value, 1)) break;
        active_frames = u32value;

        if((passive_frames > UINT8_MAX) || (active_frames > UINT8_MAX) ||
           ((passive_frames + active_frames) == 0) ||
           ((passive_frames + active_frames) > UINT8_MAX)) {
            FURI_LOG_E(TAG, "Invalid animation frame counts");
            break;
        }

        animation->passive_frames = passive_frames;
        animation->active_frames = active_frames;

        uint32_t frames = passive_frames + active_frames;
        uint32_t count = 0;
        if(!flipper_format_get_value_count(ff, "Frames order", &count)) break;
        if(count != frames) {
            FURI_LOG_E(TAG, "Error loading animation: frames order");
            break;
        }
        u32array = malloc(sizeof(uint32_t) * frames);
        if(!flipper_format_read_uint32(ff, "Frames order", u32array, frames)) break;
        animation->frame_order = malloc(sizeof(uint8_t) * frames);
        for(int i = 0; i < frames; ++i) {
            FURI_CONST_ASSIGN(animation->frame_order[i], u32array[i]);
        }

        /* passive and active frames must be loaded up to this point */
        if(!animation_storage_load_frames(
               storage,
               furi_string_get_cstr(directory),
               name,
               animation,
               u32array,
               width,
               height))
            break;

        if(!flipper_format_read_uint32(ff, "Active cycles", &u32value, 1)) break; //-V779
        if(u32value > UINT8_MAX) break;
        animation->active_cycles = u32value;
        if(!flipper_format_read_uint32(ff, "Frame rate", &u32value, 1)) break;
        u32value = momentum_animation_frame_rate(u32value, momentum_settings.anim_speed);
        FURI_CONST_ASSIGN(
            animation->icon_animation.frame_rate, MIN(u32value, (uint32_t)UINT8_MAX));
        if(!flipper_format_read_uint32(ff, "Duration", &u32value, 1)) break;
        if(u32value > UINT16_MAX) break;
        animation->duration = u32value;
        if(!flipper_format_read_uint32(ff, "Active cooldown", &u32value, 1)) break;
        if(u32value > UINT16_MAX) break;
        animation->active_cooldown = u32value;

        if(!animation_storage_load_bubbles(animation, ff)) break;
        success = true;
    } while(0);

    furi_string_free(directory);
    furi_string_free(str);
    flipper_format_free(ff);
    if(u32array) {
        free(u32array);
    }

    if(!success) animation_storage_free_animation(&animation);

    return animation;
}

static void animation_storage_free_bubbles(BubbleAnimation* animation) {
    if(!animation->frame_bubble_sequences) return;

    for(int i = 0; i < animation->frame_bubble_sequences_count;) {
        const FrameBubble* const* bubble = &animation->frame_bubble_sequences[i];

        if((*bubble) == NULL) break;

        while((*bubble)->next_bubble != NULL) {
            bubble = &(*bubble)->next_bubble;
        }

        if((*bubble)->bubble.text) {
            free((void*)(*bubble)->bubble.text);
        }
        if((*bubble) == animation->frame_bubble_sequences[i]) {
            ++i;
        }
        free((void*)*bubble);
        FURI_CONST_ASSIGN_PTR(*bubble, NULL);
    }
    free((void*)animation->frame_bubble_sequences);
    animation->frame_bubble_sequences = NULL;
}
