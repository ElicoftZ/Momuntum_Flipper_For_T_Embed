"""Run production subscription/notification dispatch with synchronous NimBLE callbacks.

Run with ESP-IDF Python from an MSVC developer shell. A non-recursive mutex
stub asserts on re-entry instead of hanging. Unrelated GAP cases are omitted.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
source = (root / 'components/ble_serial/ble_serial.c').read_text(encoding='utf-8')
start = source.index('static int serial_gap_event(')
switch = source.index('    switch(event->type)', start)
subscribe = source.index('    case BLE_GAP_EVENT_SUBSCRIBE:', switch)
mtu = source.index('    case BLE_GAP_EVENT_MTU:', subscribe)
dispatch = source[start:switch] + '    switch(event->type) {\n' + source[subscribe:mtu]
dispatch += '    default: break;\n    }\n    serial_unlock_global();\n    return 0;\n}\n'

fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#define TAG "test"
#define ESP_LOGE(...) ((void)0)
enum { BLE_GAP_EVENT_SUBSCRIBE, BLE_GAP_EVENT_NOTIFY_TX };
enum { SerialServiceEventTypeDataSent };
#define BLE_HS_EDONE 14
struct ble_gap_event {
    int type;
    struct { int attr_handle, cur_indicate, cur_notify; } subscribe;
    struct { int attr_handle, indication, status; } notify_tx;
};
typedef struct { int event; } SerialServiceEvent;
typedef void (*SerialServiceEventCallback)(SerialServiceEvent, void*);
typedef struct { SerialServiceEventCallback event_callback; void* event_context; } BleSerial;
static struct {
    BleSerial* active;
    bool tx_indicate_enabled, flow_notify_enabled, rpc_notify_enabled;
    int tx_handle, flow_handle, rpc_handle;
} serial_state;
static bool locked;
static int notifications, completed;
static void serial_lock_global(void) { assert(!locked); locked = true; }
static void serial_unlock_global(void) { assert(locked); locked = false; }
static void serial_lock(BleSerial* s) { (void)s; }
static void serial_unlock(BleSerial* s) { (void)s; }
static int serial_gap_event(struct ble_gap_event*, void*);
static void notify(int handle) {
    ++notifications;
    struct ble_gap_event event = {.type = BLE_GAP_EVENT_NOTIFY_TX};
    event.notify_tx.attr_handle = handle;
    /* IDF ble_gatts_notify_custom invokes this before returning. */
    assert(serial_gap_event(&event, NULL) == 0);
}
static void serial_send_flow(BleSerial* s) { (void)s; notify(serial_state.flow_handle); }
static void serial_send_rpc(BleSerial* s) { (void)s; notify(serial_state.rpc_handle); }
static void sent(SerialServiceEvent event, void* context) {
    assert(event.event == SerialServiceEventTypeDataSent);
    assert(context == &completed);
    ++completed;
}
'''
fixture += dispatch
fixture += r'''
int main(void) {
    BleSerial serial = {.event_callback = sent, .event_context = &completed};
    serial_state.active = &serial;
    serial_state.tx_handle = 1;
    serial_state.flow_handle = 2;
    serial_state.rpc_handle = 3;
    struct ble_gap_event e = {.type = BLE_GAP_EVENT_SUBSCRIBE};
    e.subscribe.cur_notify = 1;
    e.subscribe.attr_handle = 2;
    serial_gap_event(&e, NULL);
    e.subscribe.attr_handle = 3;
    serial_gap_event(&e, NULL);
    assert(notifications == 2 && !locked && completed == 0);
    e.subscribe.cur_notify = 0;
    serial_gap_event(&e, NULL);
    assert(notifications == 2 && !locked);
    e.type = BLE_GAP_EVENT_NOTIFY_TX;
    e.notify_tx.attr_handle = 1;
    e.notify_tx.indication = 1;
    /* Queued is not confirmed; advancing here corrupts multi-chunk frames. */
    serial_gap_event(&e, NULL);
    assert(completed == 0 && !locked);
    e.notify_tx.status = BLE_HS_EDONE;
    serial_gap_event(&e, NULL);
    assert(completed == 1 && !locked);
    e.notify_tx.status = 7;
    serial_gap_event(&e, NULL);
    assert(completed == 1 && !locked);
    puts("PASS: notifications do not deadlock; TX advances only after indication acknowledgment");
}
'''
out = root / 'build_host/ble_serial_gap'
out.mkdir(parents=True, exist_ok=True)
test = out / 'test.c'
test.write_text(fixture, encoding='utf-8')
exe = out / 'test.exe'
subprocess.run(['cl', '/nologo', '/std:c11', str(test),
                '/Fo' + str(out) + '/', '/Fe' + str(exe)], check=True)
subprocess.run([str(exe)], check=True)
