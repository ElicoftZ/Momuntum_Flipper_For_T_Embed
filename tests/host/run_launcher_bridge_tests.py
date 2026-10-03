"""Exercise the production Launcher handshake and reboot with mocked flash/NVS."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
source = (root / 'components/multiboot/launcher_bridge.c').read_text()
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define ESP_OK 0
#define ESP_FAIL 1
#define ESP_ERR_NOT_FOUND 2
#define ESP_ERR_NO_MEM 3
#define ESP_PARTITION_TYPE_APP 0
#define ESP_PARTITION_SUBTYPE_APP_FACTORY 0
#define ESP_PARTITION_SUBTYPE_APP_OTA_0 0x10
#define ESP_PARTITION_SUBTYPE_APP_OTA_15 0x1f
#define NVS_READONLY 0
#define NVS_READWRITE 1
typedef int esp_err_t;
typedef int nvs_handle_t;
typedef struct { int subtype; uint32_t address; } esp_partition_t;
typedef struct { uint8_t app_elf_sha256[32]; } esp_app_desc_t;
static esp_partition_t host = {0, 0x10000}, running = {0x10, 0x1a0000};
static int missing_host, missing_nvs, corrupt_sha, bad_protocol, bad_address;
static int write_error, commit_error, boot_error, return_flag, restarted, boot_calls;
static const esp_partition_t* esp_ota_get_running_partition(void) {return &running;}
static const esp_partition_t* esp_partition_find_first(int type, int subtype, const char* label) {
    assert(type == 0 && subtype == 0 && !strcmp(label,"launcher"));
    return missing_host ? NULL : &host;
}
static int nvs_open(const char* ns, int mode, int* handle) {
    (void)mode; assert(!strcmp(ns,"flipper_host")); *handle=1; return missing_nvs;
}
static void nvs_close(int handle) {(void)handle;}
static int nvs_get_u8(int h,const char* key,uint8_t* value) {
    (void)h; assert(!strcmp(key,"protocol")); *value=bad_protocol ? 2 : 1;return 0;
}
static int nvs_get_u32(int h,const char* key,uint32_t* value) {
    (void)h; assert(!strcmp(key,"address")); *value=bad_address ? 0x20000 : host.address;return 0;
}
static int nvs_get_blob(int h,const char* key,void* value,size_t* length) {
    (void)h;assert(!strcmp(key,"sha256") && *length==32);memset(value,corrupt_sha,32);return 0;
}
static int esp_ota_get_partition_description(const esp_partition_t* p,esp_app_desc_t* desc) {
    assert(p==&host);memset(desc,0,sizeof(*desc));return 0;
}
static int nvs_set_u8(int h,const char* key,uint8_t value) {
    (void)h;assert(!strcmp(key,"return") && value==1);if(!write_error)return_flag=value;return write_error;
}
static int nvs_commit(int h) {(void)h;return commit_error;}
static int nvs_erase_key(int h,const char* key) {(void)h;assert(!strcmp(key,"return"));return_flag=0;return 0;}
static int esp_ota_set_boot_partition(const esp_partition_t* p) {assert(p==&host);boot_calls++;return boot_error;}
static void esp_restart(void) {restarted++;}
typedef struct {int32_t (*callback)(void*);void* context;} FuriThread;
static FuriThread thread;
static FuriThread* furi_thread_alloc_ex(const char* name,int stack,int32_t (*callback)(void*),void* context) {
    assert(!strcmp(name,"LauncherReturn") && stack>=4096);thread.callback=callback;thread.context=context;return &thread;
}
static void furi_thread_start(FuriThread* t) {t->callback(t->context);}
static void furi_thread_join(FuriThread* t) {(void)t;}
static void furi_thread_free(FuriThread* t) {(void)t;}
'''
fixture += source
fixture += r'''
int main(void) {
    assert(launcher_partition()==&host);
    missing_host=1;assert(!launcher_partition());missing_host=0;
    missing_nvs=1;assert(!launcher_partition());missing_nvs=0;
    corrupt_sha=1;assert(!launcher_partition());corrupt_sha=0;
    bad_protocol=1;assert(!launcher_partition());bad_protocol=0;
    bad_address=1;assert(!launcher_partition());bad_address=0;
    running.subtype=0;assert(!launcher_partition());running.subtype=0x20;assert(!launcher_partition());running.subtype=0x10;
    assert(launcher_bridge_is_hosted());
    write_error=1;assert(launcher_bridge_return()!=0);assert(!restarted && !boot_calls && !return_flag);write_error=0;
    commit_error=1;assert(launcher_bridge_return()!=0);assert(!restarted && !boot_calls && !return_flag);commit_error=0;
    boot_error=1;assert(launcher_bridge_return()!=0);assert(!restarted && boot_calls==1 && !return_flag);boot_error=0;
    assert(launcher_bridge_return()==0);assert(restarted==1 && boot_calls==2 && return_flag==1);
    assert(running.address==0x1a0000 && host.address==0x10000);
    puts("PASS: Launcher identity, standalone detection, NVS failures, boot validation failure, return reboot");
}
'''
out = root / 'build_host/launcher_bridge'
out.mkdir(parents=True, exist_ok=True)
(out / 'test.c').write_text(fixture)
subprocess.run(['cl', '/nologo', '/W4', str(out / 'test.c'), '/Fe:' + str(out / 'test.exe'), '/Fo:' + str(out / 'test.obj')], check=True)
subprocess.run([str(out / 'test.exe')], check=True)
