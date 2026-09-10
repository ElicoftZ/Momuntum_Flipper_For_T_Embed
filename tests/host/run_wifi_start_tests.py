"""Exercise the production startup/recovery branches with simulated radios.

Run from an MSVC developer shell using ESP-IDF's Python.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
source = (root / 'components/wifi/wlan_hal.c').read_text(encoding='utf-8')


def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
#define RECORD_BT "bt"
typedef int Bt;
static Bt fake_bt;
static bool s_started, s_bt_suspended, s_user_enabled;
static bool ble_enabled, record_exists, attempts[2], can_suspend;
static void* s_cmd_queue;
static int tries, released, reserved, stops, shutdowns, restores, worker_frees;
static void wlan_auth_memory_release(void) { ++released; }
static void wlan_auth_memory_reserve(void) { ++reserved; }
static bool wlan_start_attempt(void) { assert(tries < 2); return attempts[tries++]; }
static void wlan_hal_stop_internal(bool deinit) { stops += deinit ? 1 : 10; s_started=false; }
static bool wlan_suspend_ble_for_memory(void) { s_bt_suspended=can_suspend; return can_suspend; }
static bool furi_record_exists(const char* name) { (void)name; return record_exists; }
static Bt* furi_record_open(const char* name) { (void)name; return &fake_bt; }
static void furi_record_close(const char* name) { (void)name; }
static bool bt_is_enabled(Bt* service) { (void)service; return ble_enabled; }
static void bt_start_stack(Bt* service) { (void)service; ++restores; }
static void wlan_hal_cancel_boot_time_sync(void) {}
static void wlan_hal_cancel_manual_time_sync(void) {}
static void wlan_release_worker(void) { ++worker_frees; }
static void wlan_hal_power_down(void);
'''
fixture += function('static void wlan_restore_ble(void)')
fixture += r'''
static void wlan_hal_power_down(void) {
    ++shutdowns;
    s_started=false;
    wlan_restore_ble();
}
'''
fixture += function('bool wlan_hal_start(void)')
fixture += function('void wlan_hal_finish_foreground_session(void)')
fixture += r'''
static void reset(void) {
    s_started=s_bt_suspended=s_user_enabled=false;
    ble_enabled=record_exists=can_suspend=true;
    attempts[0]=attempts[1]=false;
    s_cmd_queue=&fake_bt;
    tries=released=reserved=stops=shutdowns=restores=worker_frees=0;
}
int main(void) {
    reset(); attempts[0]=true;
    assert(wlan_hal_start() && s_started && !s_bt_suspended);
    assert(tries==1 && released==1 && reserved==1 && stops==0);
    assert(wlan_hal_start() && tries==1); /* Idempotent */
    reset(); attempts[1]=true;
    assert(wlan_hal_start() && s_bt_suspended);
    assert(tries==2 && stops==1 && shutdowns==0);
    wlan_hal_finish_foreground_session();
    assert(worker_frees==1 && restores==0 && s_bt_suspended);
    wlan_restore_ble();
    assert(restores==1 && !s_bt_suspended);
    reset(); can_suspend=false;
    assert(!wlan_hal_start() && tries==1 && shutdowns==1 && restores==0);
    reset(); /* Both attempts fail: return BLE to user */
    assert(!wlan_hal_start() && tries==2 && shutdowns==1 && restores==1);
    reset(); s_cmd_queue=NULL; attempts[1]=true;
    assert(wlan_hal_start() && tries==2 && stops==0); /* Worker allocation failed */
    reset(); s_bt_suspended=true; ble_enabled=false;
    wlan_restore_ble();
    assert(restores==0 && !s_bt_suspended); /* User disabled Bluetooth */
    reset(); s_bt_suspended=s_user_enabled=s_started=true;
    wlan_hal_finish_foreground_session();
    assert(stops==10 && worker_frees==0 && restores==0 && s_bt_suspended);
    puts("PASS: WiFi startup, BLE fallback, failed retry cleanup, deferred restore, persistent WiFi");
    return 0;
}
'''
out = root / 'build_host/wifi_start'
out.mkdir(parents=True, exist_ok=True)
test = out / 'wifi_start_test.c'
test.write_text(fixture)
exe = out / 'wifi_start_test.exe'
subprocess.run(['cl', '/nologo', '/std:c11', '/W4', '/WX', str(test),
                '/Fo' + str(out) + '/', '/Fe' + str(exe)], check=True)
subprocess.run([str(exe)], check=True)
