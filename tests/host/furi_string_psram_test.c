#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <esp_heap_caps.h>
#include <furi_string.h>

static bool psram_available = true;
static unsigned psram_requests;
static unsigned fallback_requests;

void* heap_caps_realloc(void* ptr, size_t size, uint32_t caps) {
    if(caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)) {
        psram_requests++;
        if(!psram_available && size) return NULL;
    } else {
        assert(caps == MALLOC_CAP_DEFAULT);
        fallback_requests++;
    }
    return realloc(ptr, size);
}

int main(void) {
    const char* path = "/ext/apps/Tools/a_long_application_name_for_the_browser.fap";
    FuriString* name = furi_string_alloc_set_str(path);
    assert(psram_requests >= 2); /* object and non-inline character buffer */
    assert(fallback_requests == 0);
    assert(strcmp(furi_string_get_cstr(name), path) == 0);

    FuriString* copy = furi_string_alloc_set(name);
    furi_string_reserve(copy, 1024);
    assert(fallback_requests == 0);
    assert(strcmp(furi_string_get_cstr(copy), path) == 0);

    /* Existing strings must survive expansion after PSRAM becomes full. */
    psram_available = false;
    furi_string_reserve(name, 2048);
    assert(fallback_requests > 0);
    furi_string_cat_str(name, ".backup");
    assert(furi_string_end_with_str(name, ".fap.backup"));
    assert(strncmp(furi_string_get_cstr(name), path, strlen(path)) == 0);

    /* Also cover boards with no PSRAM from their first allocation. */
    FuriString* fallback = furi_string_alloc_printf("%s %d", "Counter", 42);
    assert(strcmp(furi_string_get_cstr(fallback), "Counter 42") == 0);
    furi_string_reset(fallback);
    assert(furi_string_empty(fallback));
    furi_string_free(fallback);
    furi_string_free(copy);
    furi_string_free(name);
    puts("FuriString PSRAM preference, growth, fallback, copy and cleanup passed");
    return 0;
}
