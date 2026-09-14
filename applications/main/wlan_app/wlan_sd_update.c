#include "wlan_sd_update.h"

#include <furi.h>
#include <storage/storage.h>
#include <string.h>
#include <stdlib.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <miniz.h>

#define SD_UPDATE_TAG "WlanSdUpdate"
// Ein einziges Archiv statt eines gespiegelten Dateibaums: die Karte hat ~3700
// Dateien, und per-Datei-Download zahlte pro Datei einen TLS-Handshake.
// Zwei wählbare Quellen (siehe wlan_sd_update_set_source): das Sor3nt-Upstream
// und der Momuntum-Fork, beide im selben Release-Layout gehostet.
#define SD_UPDATE_BASE_URL_SOR3NT "https://sor3nt.github.io/release/t-embed/latest"
#define SD_UPDATE_BASE_URL_MOMUNTUM \
    "https://elicoftz.github.io/Momuntum_Flipper_For_T_Embed/release/t-embed/latest"
#define SD_UPDATE_LOCAL_VERSION "/ext/version.txt"
#define SD_UPDATE_LOCAL_ZIP "/ext/update/sdcard.zip"
#define SD_UPDATE_DEST_ROOT "/ext"
#define SD_UPDATE_CHUNK 8192
// Anzahl Versuche pro Datei bei Read-Timeout/Verbindungsabbruch. Jeder Retry
// setzt per HTTP-Range an der bereits geschriebenen Byte-Position fort.
#define SD_UPDATE_MAX_RETRY 4
// Groesster komprimierter Eintrag der Karte liegt bei ~1,7 MB; Puffer im PSRAM.
#define SD_UPDATE_MAX_ENTRY (2u * 1024u * 1024u)
#define SD_UPDATE_MAX_CD (2u * 1024u * 1024u)

static void* sd_malloc(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    return p ? p : malloc(n);
}

struct WlanSdUpdate {
    TaskHandle_t task;
    volatile WlanSdUpdatePhase phase;
    volatile uint8_t percent;
    volatile bool cancel;
    volatile bool running;
    volatile uint32_t speed_kbps;
    volatile uint32_t done_files;
    volatile uint32_t total_files;
    char current_file[64];
    char err[64];
    bool source_sor3nt; // false = Momuntum (Default), true = Sor3nt-Upstream
};

static const char* sd_update_base_url(const WlanSdUpdate* u) {
    return u->source_sor3nt ? SD_UPDATE_BASE_URL_SOR3NT : SD_UPDATE_BASE_URL_MOMUNTUM;
}

static void sd_update_set_file(WlanSdUpdate* u, const char* name) {
    strncpy(u->current_file, name, sizeof(u->current_file) - 1);
    u->current_file[sizeof(u->current_file) - 1] = '\0';
}

static void sd_update_fail(WlanSdUpdate* u, const char* msg) {
    strncpy(u->err, msg, sizeof(u->err) - 1);
    u->err[sizeof(u->err) - 1] = '\0';
    u->phase = WlanSdUpdateError;
    FURI_LOG_E(SD_UPDATE_TAG, "%s", msg);
}

// Schneidet führende/abschließende Whitespaces (inkl. \r\n) in-place ab.
static void sd_update_trim(char* s) {
    size_t n = strlen(s);
    while(n > 0 && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' ' ||
                    s[n - 1] == '\t')) {
        s[--n] = '\0';
    }
    size_t start = 0;
    while(s[start] == ' ' || s[start] == '\t' || s[start] == '\r' ||
          s[start] == '\n') {
        start++;
    }
    if(start) memmove(s, s + start, strlen(s + start) + 1);
}

static void sd_update_http_cfg(esp_http_client_config_t* cfg, const char* url) {
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = 40000;
    cfg->transport_type = HTTP_TRANSPORT_OVER_SSL;
    // SSL-Verifikation deaktiviert (kein CA gesetzt; benötigt
    // CONFIG_ESP_TLS_INSECURE / SKIP_SERVER_CERT_VERIFY).
    cfg->skip_cert_common_name_check = true;
    cfg->crt_bundle_attach = NULL;
    cfg->use_global_ca_store = false;
    cfg->buffer_size = SD_UPDATE_CHUNK;
    cfg->buffer_size_tx = 1024;
    cfg->keep_alive_enable = true;
}

// Lädt eine kleine Text-Resource synchron in out (nul-terminiert).
static bool sd_update_http_get_text(const char* url, char* out, size_t out_sz) {
    esp_http_client_config_t cfg;
    sd_update_http_cfg(&cfg, url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if(!client) return false;

    bool ok = false;
    if(esp_http_client_open(client, 0) == ESP_OK) {
        esp_http_client_fetch_headers(client);
        if(esp_http_client_get_status_code(client) == 200) {
            size_t len = 0;
            while(len + 1 < out_sz) {
                int r = esp_http_client_read(client, out + len, out_sz - 1 - len);
                if(r < 0) {
                    len = 0;
                    break;
                }
                if(r == 0) break;
                len += (size_t)r;
            }
            out[len] = '\0';
            ok = len > 0;
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static bool sd_update_read_local_version(char* out, size_t out_sz) {
    Storage* st = furi_record_open(RECORD_STORAGE);
    File* f = storage_file_alloc(st);
    bool ok = false;
    if(storage_file_open(f, SD_UPDATE_LOCAL_VERSION, FSAM_READ, FSOM_OPEN_EXISTING)) {
        size_t r = storage_file_read(f, out, out_sz - 1);
        out[r] = '\0';
        ok = true;
    }
    storage_file_close(f);
    storage_file_free(f);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

// true → lokale version.txt existiert und ist identisch mit der Remote-Version.
static bool sd_update_is_up_to_date(const char* version_url) {
    char remote[64];
    char local[64];
    if(!sd_update_http_get_text(version_url, remote, sizeof(remote))) {
        return false;
    }
    if(!sd_update_read_local_version(local, sizeof(local))) {
        return false;
    }
    sd_update_trim(remote);
    sd_update_trim(local);
    return remote[0] != '\0' && strcmp(remote, local) == 0;
}

static bool sd_update_file_present(Storage* storage, const char* path) {
    FileInfo fi;
    return storage_common_stat(storage, path, &fi) == FSE_OK && fi.size > 0;
}

// A sync interrupted mid-write (e.g. the board reset while the host still held
// the card over USB mass storage) can leave version.txt intact while the
// extracted files are truncated to 0 bytes. version.txt alone would then report
// "up to date" forever and the card would never self-repair. Cheap sentinel:
// core dolphin files that every card carries and users do not delete must exist
// and be non-empty. If any is missing/empty, treat the card as needing a
// re-sync so the normal download+extract path rewrites it.
static bool sd_update_content_intact(void) {
    static const char* const sentinels[] = {
        "/ext/dolphin/manifest.txt",
        "/ext/dolphin/L1_Waves_128x50/meta.txt",
    };
    Storage* storage = furi_record_open(RECORD_STORAGE);
    bool intact = true;
    for(size_t i = 0; i < COUNT_OF(sentinels); ++i) {
        if(!sd_update_file_present(storage, sentinels[i])) {
            FURI_LOG_W(
                SD_UPDATE_TAG, "SD content check: %s missing/empty -> repair", sentinels[i]);
            intact = false;
            break;
        }
    }
    furi_record_close(RECORD_STORAGE);
    return intact;
}

// Legt /ext/a/b rekursiv an (ohne den finalen Dateinamen).
static void sd_update_mkdirs(Storage* storage, const char* path) {
    char tmp[256];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for(char* p = tmp + 1; *p; ++p) {
        if(*p == '/') {
            *p = '\0';
            storage_common_mkdir(storage, tmp);
            *p = '/';
        }
    }
}

// Ein einzelner Download-Versuch über den (wiederverwendeten) Client.
//   *resume_from: bereits lokal vorhandene Bytes; wird per HTTP-Range
//                 fortgesetzt und auf den neuen Stand mitgeführt.
//   *complete:    true, wenn der Stream vollständig bis zum Ende gelesen wurde.
// Rückgabe true nur bei vollständigem Empfang; bei Read-Timeout/Abbruch false,
// wobei *resume_from den letzten geschriebenen Stand behält (für den Retry).
static bool sd_update_download_attempt(
    WlanSdUpdate* u,
    esp_http_client_handle_t client,
    Storage* storage,
    const char* url,
    const char* dest,
    uint32_t* resume_from,
    bool* complete) {
    *complete = false;
    if(esp_http_client_set_url(client, url) != ESP_OK) return false;

    if(*resume_from > 0) {
        char range[48];
        snprintf(range, sizeof(range), "bytes=%lu-", (unsigned long)*resume_from);
        esp_http_client_set_header(client, "Range", range);
    } else {
        // Stale Range-Header vom vorherigen Versuch am Reuse-Client entfernen.
        esp_http_client_delete_header(client, "Range");
    }

    if(esp_http_client_open(client, 0) != ESP_OK) return false;

    bool ok = false;
    File* f = NULL;
    uint8_t* chunk = NULL;

    do {
        int64_t content_length = esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        // 206 = Server akzeptiert Range (Resume). 200 = voller Inhalt — auch
        // wenn wir Range gefordert haben (Server ignoriert es) → von vorn.
        bool resumed = (status == 206);
        if(status != 200 && status != 206) break;
        if(*resume_from > 0 && !resumed) *resume_from = 0;

        // Bei 206 zaehlt Content-Length nur den Rest ab dem Range-Offset.
        uint32_t expected_total =
            (content_length > 0) ? ((uint32_t)content_length + *resume_from) : 0;

        f = storage_file_alloc(storage);
        if(resumed && *resume_from > 0) {
            if(!storage_file_open(f, dest, FSAM_WRITE, FSOM_OPEN_EXISTING)) break;
            if(!storage_file_seek(f, *resume_from, true)) break;
        } else {
            if(!storage_file_open(f, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)) break;
        }

        chunk = malloc(SD_UPDATE_CHUNK);
        if(!chunk) break;

        ok = true;
        uint32_t t0 = furi_get_tick();
        uint32_t total = *resume_from; // gesamt (für Resume-Offset)
        uint32_t session = 0;          // nur dieser Versuch (für Speed)
        while(!u->cancel) {
            int r = esp_http_client_read(client, (char*)chunk, SD_UPDATE_CHUNK);
            if(r < 0) {
                ok = false; // Timeout/Reset → Versuch gescheitert, Retry folgt
                break;
            }
            if(r == 0) {
                *complete = esp_http_client_is_complete_data_received(client);
                break;
            }
            if(storage_file_write(f, chunk, (size_t)r) != (size_t)r) {
                ok = false;
                break;
            }
            total += (uint32_t)r;
            session += (uint32_t)r;
            *resume_from = total;
            if(expected_total) {
                u->percent = (uint8_t)MIN((uint64_t)total * 100u / expected_total, 100u);
            }
            uint32_t dt = furi_get_tick() - t0;
            if(dt >= 200) {
                u->speed_kbps = (uint32_t)((uint64_t)session * 1000u / 1024u / dt);
            }
        }
        if(u->cancel) ok = false;
    } while(0);

    if(chunk) free(chunk);
    if(f) {
        storage_file_close(f);
        storage_file_free(f);
    }
    esp_http_client_close(client);

    return ok && *complete;
}

// Lädt eine Einzeldatei nach dest, mit bis zu SD_UPDATE_MAX_RETRY Versuchen.
// Bei Read-Timeout/Verbindungsabbruch wird per HTTP-Range an der bereits
// geschriebenen Position fortgesetzt (kein kompletter Neu-Download).
static bool sd_update_download_file(
    WlanSdUpdate* u,
    esp_http_client_handle_t client,
    Storage* storage,
    const char* url,
    const char* dest) {
    sd_update_mkdirs(storage, dest);

    uint32_t resume_from = 0;
    bool complete = false;

    for(int attempt = 0; attempt < SD_UPDATE_MAX_RETRY && !u->cancel; attempt++) {
        if(attempt > 0) {
            FURI_LOG_W(
                SD_UPDATE_TAG,
                "retry %d/%d %s @ %lu",
                attempt,
                SD_UPDATE_MAX_RETRY - 1,
                dest,
                (unsigned long)resume_from);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        if(sd_update_download_attempt(
               u, client, storage, url, dest, &resume_from, &complete)) {
            return true;
        }
        if(u->cancel) return false;
    }
    return false;
}

// Path-Traversal-Schutz; baut /ext/<rel>. Gilt auch für Zip-Einträge, deren
// Namen aus dem Archiv stammen ("zip slip").
static bool sd_update_safe_dest(const char* rel, char* out, size_t out_sz) {
    while(*rel == '/') rel++;
    if(!*rel) return false;
    if(strstr(rel, "..")) return false;
    int n = snprintf(out, out_sz, "%s/%s", SD_UPDATE_DEST_ROOT, rel);
    return n > 0 && (size_t)n < out_sz;
}

// ---------------------------------------------------------------------------
// ZIP-Entpacker
//
// Die Karte wird als ein einziges sdcard.zip ausgeliefert (dasselbe Archiv, das
// der Web-Flasher anbietet) statt als gespiegelter Dateibaum mit Manifest. Der
// Deflate-Decoder tinfl liegt im ESP32-S3-ROM und kostet daher keinen Flash.
// Zip-Einträge sind RAW Deflate — TINFL_FLAG_PARSE_ZLIB_HEADER darf nicht
// gesetzt werden, sonst scheitert jeder Eintrag.
// ---------------------------------------------------------------------------

#define ZIP_EOCD_SIG 0x06054b50u
#define ZIP_CD_SIG 0x02014b50u
#define ZIP_LOCAL_SIG 0x04034b50u
// 64 KB maximaler Zip-Kommentar + 22 B EOCD.
#define ZIP_EOCD_MAX_SCAN (66u * 1024u)

static uint16_t zip_rd16(const uint8_t* p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t zip_rd32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

typedef struct {
    File* out;
    bool ok;
} ZipSink;

// tinfl flusht seinen internen 32-KB-Puffer hier durch; 0 bricht ab.
static int zip_put_buf(const void* buf, int len, void* user) {
    ZipSink* s = user;
    if(!s->ok) return 0;
    if(len <= 0) return 1;
    if(storage_file_write(s->out, buf, (size_t)len) != (size_t)len) {
        s->ok = false;
        return 0;
    }
    return 1;
}

static bool zip_read_at(File* f, uint32_t off, void* buf, size_t len) {
    if(!storage_file_seek(f, off, true)) return false;
    return storage_file_read(f, buf, len) == len;
}

// Sucht das End-of-Central-Directory rückwärts vom Dateiende.
static bool zip_find_eocd(
    File* f, uint32_t fsize, uint32_t* cd_off, uint32_t* cd_size, uint32_t* count) {
    uint32_t scan = fsize < ZIP_EOCD_MAX_SCAN ? fsize : ZIP_EOCD_MAX_SCAN;
    if(scan < 22) return false;

    uint8_t* buf = sd_malloc(scan);
    if(!buf) return false;

    bool ok = false;
    if(zip_read_at(f, fsize - scan, buf, scan)) {
        for(int32_t i = (int32_t)scan - 22; i >= 0; --i) {
            if(zip_rd32(buf + i) != ZIP_EOCD_SIG) continue;
            *count = zip_rd16(buf + i + 10);
            *cd_size = zip_rd32(buf + i + 12);
            *cd_off = zip_rd32(buf + i + 16);
            ok = true;
            break;
        }
    }
    free(buf);
    return ok;
}

// Entpackt einen Eintrag nach dest. cbuf wird beim ersten Aufruf angelegt und
// über alle Einträge wiederverwendet (Caller gibt ihn frei).
static bool sd_update_write_entry(
    WlanSdUpdate* u,
    Storage* storage,
    File* zf,
    uint32_t local_header,
    uint16_t method,
    uint32_t csize,
    const char* dest,
    uint8_t** cbuf) {
    uint8_t lh[30];
    if(!zip_read_at(zf, local_header, lh, sizeof(lh)) || zip_rd32(lh) != ZIP_LOCAL_SIG) {
        sd_update_fail(u, "zip entry header bad");
        return false;
    }
    // Das Extra-Feld im Local-Header darf vom Central-Directory abweichen —
    // die Datenposition muss deshalb aus dem Local-Header kommen.
    const uint32_t data = local_header + 30u + zip_rd16(lh + 26) + zip_rd16(lh + 28);

    if(csize > SD_UPDATE_MAX_ENTRY) {
        sd_update_fail(u, "zip entry too large");
        return false;
    }
    if(!*cbuf) {
        *cbuf = sd_malloc(SD_UPDATE_MAX_ENTRY);
        if(!*cbuf) {
            sd_update_fail(u, "out of memory");
            return false;
        }
    }
    if(csize && !zip_read_at(zf, data, *cbuf, csize)) {
        sd_update_fail(u, "zip read failed");
        return false;
    }

    sd_update_mkdirs(storage, dest);
    ZipSink sink = {.out = storage_file_alloc(storage), .ok = true};
    if(!storage_file_open(sink.out, dest, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_free(sink.out);
        sd_update_fail(u, "write failed");
        return false;
    }

    bool ok;
    if(method == 0) { // STORE
        ok = csize == 0 || storage_file_write(sink.out, *cbuf, csize) == csize;
    } else if(method == 8) { // DEFLATE, raw
        size_t in_size = csize;
        ok = tinfl_decompress_mem_to_callback(*cbuf, &in_size, zip_put_buf, &sink, 0) != 0 &&
             sink.ok;
    } else {
        ok = false;
    }

    storage_file_close(sink.out);
    storage_file_free(sink.out);
    if(!ok) sd_update_fail(u, "extract failed");
    return ok;
}

static bool sd_update_extract_zip(WlanSdUpdate* u, const char* zip_path) {
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* zf = storage_file_alloc(storage);
    uint8_t* cd = NULL;
    uint8_t* cbuf = NULL;
    bool ok = false;

    do {
        FileInfo fi;
        if(storage_common_stat(storage, zip_path, &fi) != FSE_OK || fi.size < 22) {
            sd_update_fail(u, "sdcard.zip missing");
            break;
        }
        if(!storage_file_open(zf, zip_path, FSAM_READ, FSOM_OPEN_EXISTING)) {
            sd_update_fail(u, "sdcard.zip open failed");
            break;
        }

        uint32_t cd_off = 0, cd_size = 0, count = 0;
        if(!zip_find_eocd(zf, (uint32_t)fi.size, &cd_off, &cd_size, &count) || count == 0 ||
           cd_size == 0 || cd_size > SD_UPDATE_MAX_CD) {
            sd_update_fail(u, "zip directory missing");
            break;
        }

        cd = sd_malloc(cd_size);
        if(!cd || !zip_read_at(zf, cd_off, cd, cd_size)) {
            sd_update_fail(u, "zip directory read failed");
            break;
        }

        storage_common_mkdir(storage, SD_UPDATE_DEST_ROOT);
        u->total_files = count;
        u->done_files = 0;

        ok = true;
        uint32_t pos = 0;
        for(uint32_t i = 0; i < count && ok && !u->cancel; ++i) {
            if(pos + 46u > cd_size || zip_rd32(cd + pos) != ZIP_CD_SIG) {
                sd_update_fail(u, "zip directory corrupt");
                ok = false;
                break;
            }
            const uint16_t method = zip_rd16(cd + pos + 10);
            const uint32_t csize = zip_rd32(cd + pos + 20);
            const uint16_t nlen = zip_rd16(cd + pos + 28);
            const uint16_t elen = zip_rd16(cd + pos + 30);
            const uint16_t clen = zip_rd16(cd + pos + 32);
            const uint32_t local_header = zip_rd32(cd + pos + 42);
            if(pos + 46u + nlen > cd_size) {
                sd_update_fail(u, "zip directory corrupt");
                ok = false;
                break;
            }

            char rel[256];
            const uint16_t rn = nlen < sizeof(rel) - 1 ? nlen : (uint16_t)(sizeof(rel) - 1);
            memcpy(rel, cd + pos + 46, rn);
            rel[rn] = '\0';
            pos += 46u + nlen + elen + clen;

            u->done_files = i + 1;
            u->percent = (uint8_t)((uint64_t)(i + 1) * 100u / count);

            if(rn == 0 || rel[rn - 1] == '/') continue; // Verzeichniseintrag
            // version.txt ist der "fertig"-Marker und wird erst nach dem
            // letzten Eintrag geschrieben — sonst sieht ein Abbruch auf halber
            // Strecke wie eine aktuelle Karte aus und wird nie wiederholt.
            if(strcmp(rel, "version.txt") == 0) continue;

            char dest[256];
            if(!sd_update_safe_dest(rel, dest, sizeof(dest))) continue;
            sd_update_set_file(u, rel);

            ok = sd_update_write_entry(
                u, storage, zf, local_header, method, csize, dest, &cbuf);
        }
        if(u->cancel) ok = false;
    } while(0);

    if(cbuf) free(cbuf);
    if(cd) free(cd);
    storage_file_close(zf);
    storage_file_free(zf);
    furi_record_close(RECORD_STORAGE);
    return ok;
}

// Der "fertig"-Marker. Bewusst zuletzt geschrieben (siehe oben).
static void sd_update_write_local_version(Storage* storage, const char* version) {
    File* f = storage_file_alloc(storage);
    if(storage_file_open(f, SD_UPDATE_LOCAL_VERSION, FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        storage_file_write(f, version, strlen(version));
        storage_file_write(f, "\n", 1);
        storage_file_close(f);
    }
    storage_file_free(f);
}

// ---------------------------------------------------------------------------
// Worker-Task
// ---------------------------------------------------------------------------

static void sd_update_finish(WlanSdUpdate* u) {
    u->running = false;
    u->task = NULL;
    vTaskDelete(NULL);
}

static void sd_update_task(void* arg) {
    WlanSdUpdate* u = arg;
    const char* base_url = sd_update_base_url(u);
    char version_url[160];
    char zip_url[160];
    snprintf(version_url, sizeof(version_url), "%s/version.txt", base_url);
    snprintf(zip_url, sizeof(zip_url), "%s/sdcard.zip", base_url);

    u->phase = WlanSdUpdateChecking;
    u->percent = 0;

    // Skip only when the version matches AND the card content is actually intact.
    // A corrupt card with a matching version.txt falls through to re-download and
    // re-extract, repairing itself instead of being wrongly reported up to date.
    if(!u->cancel && sd_update_is_up_to_date(version_url) && sd_update_content_intact()) {
        u->phase = WlanSdUpdateUpToDate;
        sd_update_finish(u);
        return;
    }
    if(u->cancel) {
        u->phase = WlanSdUpdateIdle;
        sd_update_finish(u);
        return;
    }

    char remote[64];
    if(!sd_update_http_get_text(version_url, remote, sizeof(remote))) {
        sd_update_fail(u, "version.txt fetch failed");
        sd_update_finish(u);
        return;
    }
    sd_update_trim(remote);

    u->phase = WlanSdUpdateDownloading;
    u->percent = 0;
    u->done_files = 0;
    u->total_files = 0;
    sd_update_set_file(u, "sdcard.zip");

    Storage* storage = furi_record_open(RECORD_STORAGE);
    esp_http_client_config_t cfg;
    sd_update_http_cfg(&cfg, zip_url);
    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    bool ok = client && sd_update_download_file(
                            u, client, storage, zip_url, SD_UPDATE_LOCAL_ZIP);
    if(client) esp_http_client_cleanup(client);
    furi_record_close(RECORD_STORAGE);

    if(!ok) {
        if(u->cancel) {
            u->phase = WlanSdUpdateIdle;
        } else if(u->phase != WlanSdUpdateError) {
            sd_update_fail(u, "sdcard.zip download failed");
        }
        sd_update_finish(u);
        return;
    }

    u->phase = WlanSdUpdateExtracting;
    u->percent = 0;
    u->speed_kbps = 0;
    ok = sd_update_extract_zip(u, SD_UPDATE_LOCAL_ZIP);

    Storage* st = furi_record_open(RECORD_STORAGE);
    // Das Archiv ist ~13 MB: in jedem Fall wieder entfernen, damit ein
    // gescheiterter Lauf die Karte nicht volllaufen lässt.
    storage_common_remove(st, SD_UPDATE_LOCAL_ZIP);
    if(ok && !u->cancel) sd_update_write_local_version(st, remote);
    furi_record_close(RECORD_STORAGE);

    if(ok && !u->cancel) {
        u->percent = 100;
        u->phase = WlanSdUpdateDone;
    } else if(u->cancel && u->phase != WlanSdUpdateError) {
        u->phase = WlanSdUpdateIdle;
    } else if(u->phase != WlanSdUpdateError) {
        sd_update_fail(u, "extract failed");
    }

    sd_update_finish(u);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

WlanSdUpdate* wlan_sd_update_alloc(void) {
    WlanSdUpdate* u = malloc(sizeof(WlanSdUpdate));
    u->task = NULL;
    u->phase = WlanSdUpdateIdle;
    u->percent = 0;
    u->cancel = false;
    u->running = false;
    u->speed_kbps = 0;
    u->done_files = 0;
    u->total_files = 0;
    u->err[0] = '\0';
    u->source_sor3nt = false;
    sd_update_set_file(u, "version.txt");
    return u;
}

void wlan_sd_update_free(WlanSdUpdate* u) {
    if(!u) return;
    wlan_sd_update_cancel(u);
    free(u);
}

void wlan_sd_update_set_source(WlanSdUpdate* u, bool use_sor3nt) {
    u->source_sor3nt = use_sor3nt;
}

void wlan_sd_update_start(WlanSdUpdate* u) {
    if(u->running) return;
    u->cancel = false;
    u->percent = 0;
    u->speed_kbps = 0;
    u->done_files = 0;
    u->total_files = 0;
    u->err[0] = '\0';
    sd_update_set_file(u, "version.txt");
    u->phase = WlanSdUpdateChecking;
    u->running = true;
    if(xTaskCreate(sd_update_task, "WlanSdUpd", 8192, u, 4, &u->task) != pdPASS) {
        u->running = false;
        u->task = NULL;
        sd_update_fail(u, "Task spawn failed");
    }
}

void wlan_sd_update_cancel(WlanSdUpdate* u) {
    if(!u->running) return;
    u->cancel = true;
    for(int i = 0; i < 200 && u->running; ++i) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

WlanSdUpdatePhase wlan_sd_update_get_phase(const WlanSdUpdate* u) {
    return u->phase;
}

uint8_t wlan_sd_update_get_percent(const WlanSdUpdate* u) {
    return u->percent;
}

const char* wlan_sd_update_get_error(const WlanSdUpdate* u) {
    return u->err;
}

bool wlan_sd_update_is_running(const WlanSdUpdate* u) {
    return u->running;
}

const char* wlan_sd_update_get_current_file(const WlanSdUpdate* u) {
    return u->current_file;
}

uint32_t wlan_sd_update_get_speed_kbps(const WlanSdUpdate* u) {
    return u->speed_kbps;
}

uint32_t wlan_sd_update_get_done(const WlanSdUpdate* u) {
    return u->done_files;
}

uint32_t wlan_sd_update_get_total(const WlanSdUpdate* u) {
    return u->total_files;
}
