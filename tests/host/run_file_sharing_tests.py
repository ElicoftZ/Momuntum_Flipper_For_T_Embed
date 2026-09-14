"""Exercise production settings migration and RPC module lifecycle with MSVC."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
out = root / 'build_host/file_sharing'
out.mkdir(parents=True, exist_ok=True)


def function(source, signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def run(name, source):
    path = out / (name + '.c')
    path.write_text(source, encoding='utf-8')
    exe = out / (name + '.exe')
    subprocess.run(['cl', '/nologo', '/std:c11', str(path),
                    '/Fo' + str(out) + '/', '/Fe' + str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


common = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define furi_assert assert
#define FURI_LOG_E(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define BT_SETTINGS_PATH "settings"
#define BT_SETTINGS_MAGIC 0x19
#define BT_SETTINGS_VERSION 1
'''
header = (root / 'components/btshim/bt_settings.h').read_text()
common += header[header.index('typedef struct'):header.index('void bt_settings_load')]
common += r'''
static unsigned char saved[sizeof(BtSettings)];
static int version = -1, saves, loads;
static size_t saved_size;
static bool saved_struct_load(const char* p, void* data, size_t size, int magic, int v) {
    (void)p; assert(magic == 0x19); ++loads;
    if(version != v || saved_size != size) return false;
    memcpy(data, saved, size); return true;
}
static bool saved_struct_save(const char* p, const void* data, size_t size, int magic, int v) {
    (void)p; assert(magic == 0x19); ++saves;
    version = v; saved_size = size; memcpy(saved, data, size); return true;
}
void bt_settings_save(const BtSettings* settings);
'''
for name, path, default in (
        ('esp32_settings', 'components/btshim/bt_settings.c', 'true'),
        ('upstream_settings', 'applications/services/bt/bt_settings.c', 'false')):
    production = (root / path).read_text()
    fixture = common + function(production, 'void bt_settings_load(')
    fixture += function(production, 'void bt_settings_save(')
    fixture += r'''
int main(void) {
    BtSettings s;
    bt_settings_load(&s);
    assert(s.enabled == DEFAULT_ENABLED && s.file_sharing && saves == 1);
    for(int enabled = 0; enabled <= 1; ++enabled) {
        version = 0; saved_size = sizeof(bool); saved[0] = enabled;
        bt_settings_load(&s);
        assert(s.enabled == (bool)enabled && s.file_sharing);
        assert(version == 1 && saved_size == sizeof(BtSettings));
        s.file_sharing = false;
        bt_settings_save(&s);
        int old_saves = saves;
        memset(&s, 0xff, sizeof(s));
        bt_settings_load(&s);
        assert(s.enabled == (bool)enabled && !s.file_sharing && saves == old_saves);
        s.file_sharing = true;
        bt_settings_save(&s);
        bt_settings_load(&s);
        assert(s.file_sharing);
    }
    puts("PASS: defaults, legacy On/Off migration, File Sharing Off/On persistence");
}
'''.replace('DEFAULT_ENABLED', default)
    run(name, fixture)

rpc = (root / 'components/rpc/rpc.c').read_text()
allocation = rpc[rpc.index('    bool file_sharing = true;'):rpc.index('    RpcHandler rpc_handler = {', rpc.index('RpcSession* rpc_session_open('))]
cleanup_start = rpc.index('    for(size_t i = 0;', rpc.index('static void rpc_session_thread_pending_callback'))
cleanup = rpc[cleanup_start:rpc.index('    free(session->decoded_message);', cleanup_start)]
fixture = common + r'''
#define COUNT_OF(a) (sizeof(a) / sizeof((a)[0]))
typedef enum { RpcOwnerUnknown, RpcOwnerBle, RpcOwnerUsb, RpcOwnerUart } RpcOwner;
typedef struct { void** system_contexts; } RpcSession;
static bool sharing;
static int settings_reads, allocated[5], released[5];
static void bt_settings_load(BtSettings* s) { ++settings_reads; s->file_sharing = sharing; }
static void* module_alloc(int i) { ++allocated[i]; return &allocated[i]; }
static void* system_alloc(RpcSession* s) { (void)s; return module_alloc(0); }
static void* rpc_system_storage_alloc(RpcSession* s) { (void)s; return module_alloc(1); }
static void* app_alloc(RpcSession* s) { (void)s; return module_alloc(2); }
static void* gui_alloc(RpcSession* s) { (void)s; return module_alloc(3); }
static void* property_alloc(RpcSession* s) { (void)s; return module_alloc(4); }
static void module_free(void* context) {
    assert(context);
    ++released[(int*)context - allocated];
}
static struct { void* (*alloc)(RpcSession*); void (*free)(void*); } rpc_systems[] = {
    {system_alloc, NULL}, {rpc_system_storage_alloc, module_free},
    {app_alloc, module_free}, {gui_alloc, module_free}, {property_alloc, NULL},
};
'''
fixture += 'static void open_systems(RpcSession* session, RpcOwner owner) {\n' + allocation + '}\n'
fixture += 'static void close_systems(RpcSession* session) {\n' + cleanup + '}\n'
fixture += r'''
int main(void) {
    for(int owner = RpcOwnerUnknown; owner <= RpcOwnerUart; ++owner) {
        for(int enabled = 0; enabled <= 1; ++enabled) {
            memset(allocated, 0, sizeof(allocated));
            memset(released, 0, sizeof(released));
            settings_reads = 0; sharing = enabled;
            RpcSession session;
            open_systems(&session, owner);
            bool storage = owner != RpcOwnerBle || enabled;
            assert(allocated[0] == 1 && allocated[1] == (int)storage);
            assert(allocated[2] == 1 && allocated[3] == 1 && allocated[4] == 1);
            assert(settings_reads == (owner == RpcOwnerBle));
            sharing = !sharing; /* Existing session retains its allocation. */
            assert((session.system_contexts[1] != NULL) == storage);
            close_systems(&session);
            assert(released[1] == (int)storage && released[2] == 1 && released[3] == 1);
        }
    }
    puts("PASS: BLE storage gate, all other modules/transports retained, session snapshot and cleanup");
}
'''
run('rpc_modules', fixture)
