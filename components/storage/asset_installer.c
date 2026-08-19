/**
 * @file asset_installer.c
 * @brief First-boot install of the SD content this port adds.
 *
 * Release images carry a tar in the `assets` partition holding only the delta
 * this port introduces over an ordinary Flipper card: its own .fap apps, the
 * welcome slideshow, and the folders its apps expect. On boot this compares the
 * version baked into that tar against `version.txt` on the card and extracts
 * only when they differ.
 *
 * Deliberate rule: **an existing version.txt means the card is already set up,
 * so leave it alone.** Someone who has customised their card must never have it
 * overwritten by a firmware that thinks it knows better. That makes the file
 * the single source of truth for whether the install already happened.
 *
 * Absent on dual-boot/personal builds: those have no `assets` partition, and
 * asset_installer_run() returns quietly rather than treating that as an error.
 */

#include "asset_installer.h"

#include <furi.h>
#include <storage/storage.h>
#include <microtar.h>

#include <esp_partition.h>

#include <stdlib.h>
#include <string.h>

#define TAG "AssetInstall"

#define ASSET_PARTITION_LABEL "assets"
#define ASSET_VERSION_PATH    EXT_PATH("version.txt")
#define ASSET_VERSION_MAX     64
/* Copy buffer. Small: this runs once, and boot is not the time to hold a big
 * allocation hostage. */
#define ASSET_COPY_CHUNK 1024

/* This microtar has no memory reader, but mtar_init() takes custom stream
 * ops -- so the mmap'd partition is fed in directly, with no copy. */
typedef struct {
    const uint8_t* base;
    size_t size;
    size_t pos;
} AssetMemStream;

/* microtar's tread() advances its position by the RETURN VALUE and checks it
 * against the requested size, so read returns a byte count, not a status. */
static int asset_mem_read(void* stream, void* data, unsigned size) {
    AssetMemStream* m = stream;
    if(m->pos + size > m->size) return MTAR_EREADFAIL;
    memcpy(data, m->base + m->pos, size);
    m->pos += size;
    return (int)size;
}

static int asset_mem_seek(void* stream, unsigned pos) {
    AssetMemStream* m = stream;
    if(pos > m->size) return MTAR_ESEEKFAIL;
    m->pos = pos;
    return MTAR_ESUCCESS;
}

static int asset_mem_close(void* stream) {
    UNUSED(stream);
    return MTAR_ESUCCESS;
}

static const mtar_ops_t asset_mem_ops = {
    .read = asset_mem_read,
    .write = NULL, /* read-only */
    .seek = asset_mem_seek,
    .close = asset_mem_close,
};

/** Read version.txt from the card. @return length, or 0 when absent/unreadable. */
static size_t asset_read_card_version(Storage* storage, char* out, size_t out_size) {
    File* file = storage_file_alloc(storage);
    size_t len = 0;

    if(storage_file_open(file, ASSET_VERSION_PATH, FSAM_READ, FSOM_OPEN_EXISTING)) {
        const size_t got = storage_file_read(file, out, out_size - 1);
        out[got] = '\0';
        /* Trim trailing newline/space so a hand-edited file still compares. */
        len = got;
        while(len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r' || out[len - 1] == ' ')) {
            out[--len] = '\0';
        }
    }

    storage_file_close(file);
    storage_file_free(file);
    return len;
}

/** Create every parent directory of a tar entry path. */
static void asset_make_parents(Storage* storage, const char* rel) {
    char path[256];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_EXT_PATH_PREFIX, rel);

    for(char* p = path + strlen(STORAGE_EXT_PATH_PREFIX) + 1; *p; p++) {
        if(*p != '/') continue;
        *p = '\0';
        storage_simply_mkdir(storage, path);
        *p = '/';
    }
}

/** Write the completion marker. Deliberately the LAST thing this does: the file
 * means "the install finished", so a power cut mid-extract must leave it absent
 * and let the next boot start over. That is also why it is skipped during the
 * extract loop even though it is the first entry in the archive. */
static bool asset_write_version(Storage* storage, const char* version) {
    File* file = storage_file_alloc(storage);
    bool ok = storage_file_open(file, ASSET_VERSION_PATH, FSAM_WRITE, FSOM_CREATE_ALWAYS);

    if(ok) {
        const size_t len = strlen(version);
        ok = storage_file_write(file, version, len) == len;
        if(ok) ok = storage_file_write(file, "\n", 1) == 1;
    }

    storage_file_close(file);
    storage_file_free(file);
    return ok;
}

static bool asset_write_entry(Storage* storage, mtar_t* tar, const mtar_header_t* h) {
    asset_make_parents(storage, h->name);

    char path[256];
    snprintf(path, sizeof(path), "%s/%s", STORAGE_EXT_PATH_PREFIX, h->name);

    File* out = storage_file_alloc(storage);
    bool ok = storage_file_open(out, path, FSAM_WRITE, FSOM_CREATE_ALWAYS);

    if(ok) {
        uint8_t* buf = malloc(ASSET_COPY_CHUNK);
        size_t left = h->size;
        while(ok && left > 0) {
            const size_t n = left > ASSET_COPY_CHUNK ? ASSET_COPY_CHUNK : left;
            /* Both calls report a byte count; a short read or write is a fault. */
            ok = (mtar_read_data(tar, buf, (unsigned)n) == (int)n) &&
                 (storage_file_write(out, buf, n) == n);
            left -= n;
        }
        free(buf);
    }

    storage_file_close(out);
    storage_file_free(out);

    if(!ok) FURI_LOG_E(TAG, "failed writing %s", h->name);
    return ok;
}

bool asset_installer_run(void) {
    const esp_partition_t* part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, ASSET_PARTITION_LABEL);
    if(!part) {
        /* Personal/dual-boot build: nothing to install, and that is not a fault. */
        return false;
    }

    Storage* storage = furi_record_open(RECORD_STORAGE);

    if(storage_sd_status(storage) != FSE_OK) {
        /* No card to install onto. Bail before the mmap rather than logging a
         * failure for every entry in the archive. */
        FURI_LOG_W(TAG, "no SD card, skipping install");
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    /* Map the partition so the tar can be read as ordinary memory. */
    const void* tar_data = NULL;
    esp_partition_mmap_handle_t map = 0;
    if(esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &tar_data, &map) !=
       ESP_OK) {
        FURI_LOG_E(TAG, "cannot map assets partition");
        furi_record_close(RECORD_STORAGE);
        return false;
    }

    /* Declared up front: the `goto done` paths below must not jump over an
     * initialiser. */
    bool installed = false;
    bool all_ok = true;
    size_t files = 0;
    unsigned n = 0;
    char want[ASSET_VERSION_MAX] = {0};
    char have[ASSET_VERSION_MAX] = {0};
    mtar_t tar;
    const mtar_header_t* header;

    AssetMemStream mem = {.base = (const uint8_t*)tar_data, .size = part->size, .pos = 0};
    mtar_init(&tar, MTAR_READ, &asset_mem_ops, &mem);

    /* version.txt is packed first precisely so this costs one header read.
     * mtar_next() parses a header, mtar_get_header() hands back the parse. */
    if(mtar_next(&tar) != MTAR_ESUCCESS) {
        /* An unflashed partition reads back as 0xFF and fails here. That is the
         * normal state after a firmware-only flash, so it is not an error. */
        FURI_LOG_W(TAG, "no readable archive in assets partition");
        goto done;
    }

    header = mtar_get_header(&tar);
    if(!header || strcmp(header->name, "version.txt") != 0) {
        FURI_LOG_W(TAG, "archive does not start with version.txt");
        goto done;
    }

    n = header->size < sizeof(want) - 1 ? header->size : (unsigned)(sizeof(want) - 1);
    mtar_read_data(&tar, want, n);
    for(size_t i = 0; i < sizeof(want); i++) {
        if(want[i] == '\n' || want[i] == '\r') {
            want[i] = '\0';
            break;
        }
    }

    if(asset_read_card_version(storage, have, sizeof(have)) > 0) {
        /* Present at all means installed. Not compared for equality on purpose:
         * a user who edited it has told us to keep our hands off the card. */
        FURI_LOG_I(TAG, "card already set up (version.txt = \"%s\")", have);
        goto done;
    }

    FURI_LOG_I(TAG, "fresh card: installing assets (version %s)", want);

    mtar_rewind(&tar);
    /* mtar_next() skips whatever data the previous entry left unread, so the
     * loop needs no explicit advance after writing a file. */
    while(mtar_next(&tar) == MTAR_ESUCCESS) {
        const mtar_header_t* h = mtar_get_header(&tar);
        if(!h) break;

        /* Written at the end instead, once everything else is on the card. */
        if(strcmp(h->name, "version.txt") == 0) continue;

        if(h->type == MTAR_TDIR) {
            /* tar marks a directory with a trailing slash, which storage will
             * not take -- and a nested one (backup/nvs) needs its parent made
             * first, which is exactly what asset_make_parents() walks. */
            asset_make_parents(storage, h->name);

            char path[256];
            snprintf(path, sizeof(path), "%s/%s", STORAGE_EXT_PATH_PREFIX, h->name);
            size_t len = strlen(path);
            while(len > 1 && path[len - 1] == '/') path[--len] = '\0';
            storage_simply_mkdir(storage, path);
        } else if(h->type == MTAR_TREG) {
            if(asset_write_entry(storage, &tar, h)) {
                files++;
            } else {
                all_ok = false;
            }
        }
    }

    if(!all_ok) {
        /* Leave the marker off so the next boot retries rather than shipping a
         * half-installed card that looks finished. */
        FURI_LOG_E(TAG, "install incomplete (%u files), not marking the card", (unsigned)files);
        goto done;
    }

    installed = asset_write_version(storage, want);
    FURI_LOG_I(
        TAG,
        "installed %u files, version.txt %s",
        (unsigned)files,
        installed ? "written" : "FAILED");

done:
    esp_partition_munmap(map);
    furi_record_close(RECORD_STORAGE);
    return installed;
}
