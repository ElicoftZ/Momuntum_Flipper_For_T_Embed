"""Regression: recorder worker OOM and reservation lifecycle (MSVC shell)."""
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[2]
def function(path, signature):
    src = (root / path).read_text(encoding='utf-8')
    start = src.index(signature)
    end = src.index('{', start) + 1
    depth = 1
    while depth:
        depth += (src[end] == '{') - (src[end] == '}')
        end += 1
    return src[start:end]
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#define furi_check assert
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_8BIT 2
#define THREAD_MAX_STACK_SIZE 65536
#define MAX(a,b) ((a)>(b)?(a):(b))
typedef uint32_t StackType_t;
typedef int32_t (*FuriThreadCallback)(void*);
typedef struct { void* stack_buffer; uint32_t stack_size; } FuriThread;
static int calls, fail_at, outstanding, initialized, freed, stopped;
static void* heap_caps_malloc(size_t size, int caps) {
    assert(caps == (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if(++calls == fail_at) return NULL;
    ++outstanding; return malloc(size);
}
static void* heap_caps_calloc(size_t n, size_t size, int caps) {
    assert(n == 1); return heap_caps_malloc(size, caps);
}
static void heap_caps_free(void* ptr) { assert(ptr); --outstanding; free(ptr); }
static void furi_thread_init_common(FuriThread* thread) { assert(thread); ++initialized; }
static void furi_thread_set_name(FuriThread* t, const char* n) { (void)t; (void)n; }
static void furi_thread_set_callback(FuriThread* t, FuriThreadCallback cb) { (void)t; (void)cb; }
static void furi_thread_set_context(FuriThread* t, void* ctx) { (void)t; (void)ctx; }
static void furi_thread_free(FuriThread* t) {
    heap_caps_free(t->stack_buffer); heap_caps_free(t); ++freed;
}
static FuriThread* s_writer;
static int32_t wlan_pcap_rec_writer_task(void* ctx) { (void)ctx; return 0; }
static void wlan_pcap_rec_stop(void) { ++stopped; }
'''
fixture += function('components/furi/core/thread.c', 'FuriThread* furi_thread_try_alloc_ex(')
fixture += function('applications/main/wlan_app/wlan_pcap_rec.c', 'bool wlan_pcap_rec_prepare(void)')
fixture += function('applications/main/wlan_app/wlan_pcap_rec.c', 'void wlan_pcap_rec_release(void)')
fixture += r'''
int main(void) {
    fail_at=1;
    assert(!wlan_pcap_rec_prepare());
    assert(!s_writer && !outstanding && !initialized);
    calls=0; fail_at=2;
    assert(!wlan_pcap_rec_prepare());
    assert(!s_writer && !outstanding && !initialized);
    calls=0; fail_at=0;
    assert(wlan_pcap_rec_prepare());
    assert(s_writer->stack_size==4096 && outstanding==2 && initialized==1);
    FuriThread* first=s_writer;
    assert(wlan_pcap_rec_prepare() && s_writer==first && calls==2);
    wlan_pcap_rec_stop();
    assert(wlan_pcap_rec_prepare() && s_writer==first && calls==2);
    wlan_pcap_rec_release();
    assert(!s_writer && !outstanding && freed==1 && stopped==2);
    wlan_pcap_rec_release();
    assert(!outstanding && freed==1);
    assert(wlan_pcap_rec_prepare());
    wlan_pcap_rec_release();
    assert(!outstanding && freed==2);
    puts("PASS: TCB OOM, stack OOM cleanup, retry, worker reuse, app exit/re-entry");
}
'''
out = root / 'build_host/probe_worker'
out.mkdir(parents=True, exist_ok=True)
source = out / 'test.c'
source.write_text(fixture)
exe = out / 'test.exe'
subprocess.run(['cl', '/nologo', '/std:c11', '/W4', '/WX', str(source), '/Fo'+str(out)+'/', '/Fe'+str(exe)], check=True)
subprocess.run([str(exe)], check=True)
