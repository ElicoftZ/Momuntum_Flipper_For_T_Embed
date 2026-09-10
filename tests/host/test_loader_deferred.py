"""Exercise the actual deferred-launch C functions with host service stubs."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build_host" / "loader_deferred"


def function(source, name):
    start = source.index("static ", source.rfind("\n}", 0, source.index(name)))
    brace = source.index("{", source.index(name))
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


def main():
    BUILD.mkdir(parents=True, exist_ok=True)
    source = (ROOT / "components/loader/loader.c").read_text()
    queue_header = (ROOT / "components/loader/loader_queue.h").read_text()
    queue_header = "\n".join(l for l in queue_header.splitlines() if not l.startswith("#include"))
    queue_source = (ROOT / "components/loader/loader_queue.c").read_text()
    queue_source = queue_source.replace('#include "loader_queue.h"', '')
    prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef enum {LoaderDeferredLaunchFlagGui = 1} LoaderDeferredLaunchFlag;
'''
    stubs = r'''
typedef struct { int unused; } FuriString;
typedef struct {
    struct {void* thread;} app;
    LoaderLaunchQueue launch_queue;
    void* pubsub;
} Loader;
typedef enum {LoaderStatusOk, LoaderStatusErrorInternal} LoaderStatus;
typedef struct {LoaderStatus value;} LoaderMessageLoaderStatusResult;
typedef struct {int type;} LoaderEvent;
enum {LoaderEventTypeNoMoreAppsInQueue};
#define TAG "Loader"
#define FURI_LOG_I(...) ((void)0)
#define FURI_LOG_E(...) ((void)0)
static int attempts, errors, empty_events;
static char launched[8][32];
static FuriString* furi_string_alloc(void) {return calloc(1, sizeof(FuriString));}
static void furi_string_free(FuriString* s) {free(s);}
static const char* furi_string_get_cstr(FuriString* s) {(void)s; return "error";}
static void loader_show_gui_error(FuriString* s) {(void)s; errors++;}
static void furi_pubsub_publish(void* p, LoaderEvent* e) {
    (void)p;
    assert(e->type == LoaderEventTypeNoMoreAppsInQueue);
    empty_events++;
}
static LoaderMessageLoaderStatusResult loader_do_start_by_name(
    Loader* l, const char* name, const char* args, FuriString* err) {
    (void)err;
    assert(!l->app.thread);
    assert(args == NULL || strcmp(args, "saved arguments") == 0);
    strcpy(launched[attempts++], name);
    if(strcmp(name, "bad") == 0) return (LoaderMessageLoaderStatusResult){LoaderStatusErrorInternal};
    l->app.thread = l;
    return (LoaderMessageLoaderStatusResult){LoaderStatusOk};
}
'''
    cases = r'''
static void enqueue(Loader* l, const char* name, const char* args, int gui) {
    LoaderDeferredLaunchRecord r = {_strdup(name), args ? _strdup(args) : NULL, gui};
    assert(loader_queue_push(&l->launch_queue, &r));
}
int main(void) {
    Loader l = {0};
    l.app.thread = &l; /* Sub-GHz is still running. */
    enqueue(&l, "ProtoPirate", "saved arguments", 1);
    enqueue(&l, "wM-Burst", NULL, 1);
    loader_do_next_deferred_launch(&l);
    assert(attempts == 0 && empty_events == 0 && l.launch_queue.item_cnt == 2);
    l.app.thread = NULL; /* The current app has joined and released its resources. */
    loader_do_next_deferred_launch(&l);
    assert(attempts == 1 && strcmp(launched[0], "ProtoPirate") == 0);
    assert(empty_events == 0 && l.launch_queue.item_cnt == 1);
    l.app.thread = NULL;
    loader_do_next_deferred_launch(&l);
    assert(attempts == 2 && strcmp(launched[1], "wM-Burst") == 0);
    assert(empty_events == 0);
    l.app.thread = NULL;
    loader_do_next_deferred_launch(&l);
    assert(empty_events == 1);
    /* Bad files must report their error and allow the next launch. */
    enqueue(&l, "bad", NULL, 1);
    enqueue(&l, "bad", NULL, 0);
    enqueue(&l, "valid", NULL, 1);
    loader_do_next_deferred_launch(&l);
    assert(attempts == 5 && errors == 1 && empty_events == 1);
    assert(strcmp(launched[4], "valid") == 0);
    l.app.thread = NULL;
    enqueue(&l, "bad", NULL, 1);
    loader_do_next_deferred_launch(&l);
    assert(errors == 2 && empty_events == 2 && l.app.thread == NULL);
    /* A full queue rejects another record without taking ownership. */
    for(int i=0; i<LOADER_QUEUE_MAX_SIZE; i++) enqueue(&l, "queued", NULL, 0);
    LoaderDeferredLaunchRecord rejected = {_strdup("overflow"), NULL, 0};
    assert(!loader_queue_push(&l.launch_queue, &rejected));
    loader_queue_item_clear(&rejected);
    loader_queue_clear(&l.launch_queue);
    assert(l.launch_queue.item_cnt == 0);
    puts("PASS: locked deferral, FIFO, arguments, success/failure, GUI flag, queue-empty events, capacity");
    return 0;
}
'''
    code = prelude + queue_header + queue_source + stubs
    code += function(source, 'loader_do_is_locked(Loader*')
    code += function(source, 'loader_do_next_deferred_launch(Loader*') + cases
    (BUILD / "test.c").write_text(code)
    vs = Path(os.environ.get("ProgramFiles(x86)", "C:/Program Files (x86)")) / "Microsoft Visual Studio"
    vcvars = next(vs.glob("*/BuildTools/VC/Auxiliary/Build/vcvars64.bat"))
    batch = BUILD / "run.bat"
    batch.write_text(f'@echo off\ncall "{vcvars}" >nul\n'
                     'if errorlevel 1 exit /b 1\n'
                     f'cd /d "{BUILD}"\n'
                     'cl /nologo /std:c17 /W4 /WX /D_CRT_SECURE_NO_WARNINGS test.c /Fe:test.exe\n'
                     'if errorlevel 1 exit /b 1\ntest.exe\n')
    subprocess.run(["cmd.exe", "/c", str(batch)], check=True)


if __name__ == "__main__":
    main()
