"""Run the production Chameleon transport against a simulated NimBLE peer.

Run from an MSVC developer shell. --baseline checks the pre-fix transport.
"""
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
path = 'applications/main/nfc/chameleon/chameleon.c'
source = (subprocess.check_output(['git', 'show', 'HEAD:' + path]).decode('utf-8')
          if '--baseline' in sys.argv else (root / path).read_text(encoding='utf-8'))
source = '\n'.join(line for line in source.splitlines() if not line.startswith('#include'))
source = source[:source.index('bool chameleon_mf1_write_block(')]
if '--baseline' in sys.argv:
    source = 'static uint16_t s_expected_command;\n' + source
header = (root / 'applications/main/nfc/chameleon/chameleon.h').read_text(encoding='utf-8')
fixture = r'''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_OK 0
#define BLE_HS_CONN_HANDLE_NONE 0xffff
#define BLE_HS_FOREVER -1
#define BLE_HS_IO_NO_INPUT_OUTPUT 3
#define BLE_HS_IO_KEYBOARD_ONLY 2
#define BLE_HS_ADV_TYPE_COMP_NAME 9
#define BLE_HS_ADV_TYPE_INCOMP_NAME 8
#define BLE_ERR_REM_USER_CONN_TERM 0x13
#define BLE_SM_IOACT_INPUT 2
#define BLE_ATT_ERR_INSUFFICIENT_AUTHEN 5
#define BLE_ATT_ERR_INSUFFICIENT_ENC 15
#define BLE_HS_ATT_ERR(x) (0x100 + (x))
#define BLE_UUID128_INIT(...) {{ {__VA_ARGS__} }}
#define BLE_UUID16_INIT(x) {{ {(x) & 0xff, (x) >> 8} }}
#define RECORD_BT "bt"
typedef int esp_err_t;
typedef int Bt;
typedef struct {uint8_t bytes[16];} ble_uuid_t;
typedef struct {ble_uuid_t u;} ble_uuid128_t;
typedef struct {ble_uuid_t u;} ble_uuid16_t;
typedef struct {int type; uint8_t val[6];} ble_addr_t;
struct os_mbuf {uint16_t len; uint8_t* data;};
#define OS_MBUF_PKTLEN(om) ((om)->len)
struct ble_gatt_error {int status;};
struct ble_gatt_svc {ble_uuid128_t uuid; uint16_t start_handle, end_handle;};
struct ble_gatt_chr {ble_uuid128_t uuid; uint16_t val_handle;};
struct ble_gatt_dsc {ble_uuid16_t uuid; uint16_t handle;};
struct ble_gatt_attr {int unused;};
struct ble_gap_disc_params {int passive, itvl, window, filter_duplicates;};
enum {BLE_GAP_EVENT_DISC, BLE_GAP_EVENT_DISC_COMPLETE, BLE_GAP_EVENT_CONNECT,
      BLE_GAP_EVENT_DISCONNECT, BLE_GAP_EVENT_NOTIFY_RX, BLE_GAP_EVENT_PASSKEY_ACTION,
      BLE_GAP_EVENT_ENC_CHANGE};
struct ble_gap_event {
    int type;
    struct {uint8_t* data; uint8_t length_data; ble_addr_t addr;} disc;
    struct {int status; uint16_t conn_handle;} connect;
    struct {uint16_t attr_handle; struct os_mbuf* om;} notify_rx;
    struct {struct {uint8_t action;} params; uint16_t conn_handle;} passkey;
    struct {int status;} enc_change;
};
struct ble_sm_io {uint8_t action; uint32_t passkey;};
typedef int (*gap_cb)(struct ble_gap_event*, void*);
typedef int (*write_cb)(uint16_t, const struct ble_gatt_error*, struct ble_gatt_attr*, void*);
typedef int (*mtu_cb)(uint16_t, const struct ble_gatt_error*, uint16_t, void*);
typedef int (*svc_cb)(uint16_t, const struct ble_gatt_error*, const struct ble_gatt_svc*, void*);
typedef int (*chr_cb)(uint16_t, const struct ble_gatt_error*, const struct ble_gatt_chr*, void*);
typedef int (*dsc_cb)(uint16_t, const struct ble_gatt_error*, uint16_t, const struct ble_gatt_dsc*, void*);
static void furi_delay_ms(int);
static Bt* furi_record_open(const char*);
static void furi_record_close(const char*);
static bool bt_is_enabled(Bt*);
static void bt_stop_stack(Bt*);
static void bt_start_stack(Bt*);
static int nimble_glue_init(const char*);
static void nimble_glue_configure_security(bool, bool, bool, uint8_t);
static int nimble_glue_start(void*, void*);
static void nimble_glue_stop(void);
static uint8_t nimble_glue_own_address_type(void);
static int ble_att_set_preferred_mtu(int);
static uint16_t ble_att_mtu(uint16_t);
static int ble_uuid_cmp(const ble_uuid_t*, const ble_uuid_t*);
static int ble_hs_mbuf_to_flat(struct os_mbuf*, void*, uint16_t, uint16_t*);
static int ble_gap_disc_cancel(void);
static int ble_gap_conn_cancel(void);
static int ble_gap_terminate(uint16_t, int);
static int ble_gap_disc(uint8_t, int, const struct ble_gap_disc_params*, gap_cb, void*);
static int ble_gap_connect(uint8_t, const ble_addr_t*, int, void*, gap_cb, void*);
static int ble_gattc_exchange_mtu(uint16_t, mtu_cb, void*);
static int ble_gattc_disc_all_svcs(uint16_t, svc_cb, void*);
static int ble_gattc_disc_all_chrs(uint16_t, uint16_t, uint16_t, chr_cb, void*);
static int ble_gattc_disc_all_dscs(uint16_t, uint16_t, uint16_t, dsc_cb, void*);
static int ble_gattc_write_flat(uint16_t, uint16_t, const void*, uint16_t, write_cb, void*);
static int ble_gap_security_initiate(uint16_t);
static int ble_sm_inject_io(uint16_t, const struct ble_sm_io*);
'''
fixture += header + '\n' + source
fixture += r'''
static int restores, stops, cccd_attempts, security_calls, writes;
static bool bt_enabled = true, require_pin, fail_pairing, fail_write, disconnect_write;
static bool incomplete_chars, no_device, response_pending;
static uint16_t negotiated_mtu = 23;
static uint32_t injected_pin;
static mtu_cb pending_mtu;
static uint8_t tx[600], rx[600];
static size_t tx_len, rx_len;
static Bt fake_bt;
static Bt* furi_record_open(const char* s) {(void)s; return &fake_bt;}
static void furi_record_close(const char* s) {(void)s;}
static bool bt_is_enabled(Bt* b) {(void)b; return bt_enabled;}
static void bt_stop_stack(Bt* b) {(void)b; ++stops;}
static void bt_start_stack(Bt* b) {(void)b; ++restores;}
static int nimble_glue_init(const char* s) {(void)s; return ESP_OK;}
static void nimble_glue_configure_security(bool b, bool m, bool sc, uint8_t io) {
    (void)b; (void)m; (void)sc; (void)io;
}
static int nimble_glue_start(void* a, void* b) {(void)a; (void)b; return ESP_OK;}
static void nimble_glue_stop(void) {}
static uint8_t nimble_glue_own_address_type(void) {return 0;}
static int ble_att_set_preferred_mtu(int m) {(void)m; return 0;}
static uint16_t ble_att_mtu(uint16_t h) {(void)h; return negotiated_mtu;}
static int ble_uuid_cmp(const ble_uuid_t* a, const ble_uuid_t* b) {return memcmp(a, b, sizeof(*a));}
static int ble_hs_mbuf_to_flat(struct os_mbuf* om, void* p, uint16_t n, uint16_t* copied) {
    memcpy(p, om->data, n); *copied=n; return 0;
}
static int ble_gap_disc_cancel(void) {s_scanning=false; return 0;}
static int ble_gap_conn_cancel(void) {return 0;}
static int ble_gap_terminate(uint16_t h, int reason) {
    (void)h; (void)reason;
    struct ble_gap_event e={0}; e.type=BLE_GAP_EVENT_DISCONNECT; cham_gap_cb(&e, NULL); return 0;
}
static int ble_gap_disc(uint8_t a, int t, const struct ble_gap_disc_params* p, gap_cb cb, void* ctx) {
    (void)a; (void)t; (void)p;
    if(!no_device) {
        uint8_t name[]={15, 9, 'C','h','a','m','e','l','e','o','n','U','l','t','r','a'};
        struct ble_gap_event e={0}; e.type=BLE_GAP_EVENT_DISC;
        e.disc.data=name; e.disc.length_data=sizeof(name); cb(&e, ctx);
    }
    return 0;
}
static int ble_gap_connect(uint8_t a, const ble_addr_t* d, int t, void* p, gap_cb cb, void* ctx) {
    (void)a; (void)d; (void)t; (void)p;
    struct ble_gap_event e={0}; e.type=BLE_GAP_EVENT_CONNECT; e.connect.conn_handle=1;
    cb(&e, ctx); return 0;
}
static int ble_gattc_exchange_mtu(uint16_t h, mtu_cb cb, void* ctx) {
    (void)h; (void)ctx; pending_mtu=cb; return 0;
}
static void furi_delay_ms(int ms) {
    (void)ms;
    if(pending_mtu) {
        mtu_cb cb=pending_mtu; pending_mtu=NULL;
        struct ble_gatt_error e={0}; cb(1, &e, negotiated_mtu, NULL);
    }
    if(response_pending) {
        response_pending=false;
        struct os_mbuf om={(uint16_t)rx_len, rx}; cham_receive_notification(&om);
    }
}
static int ble_gattc_disc_all_svcs(uint16_t h, svc_cb cb, void* ctx) {
    if(pending_mtu) return 1;
    struct ble_gatt_error e={0}; struct ble_gatt_svc svc={NUS_SVC_UUID, 2, 10};
    cb(h, &e, &svc, ctx); e.status=1; cb(h, &e, NULL, ctx); return 0;
}
static int ble_gattc_disc_all_chrs(uint16_t h, uint16_t start, uint16_t end, chr_cb cb, void* ctx) {
    (void)start; (void)end;
    struct ble_gatt_error e={0}; struct ble_gatt_chr chr={NUS_TX_UUID, 4};
    cb(h, &e, &chr, ctx); chr.uuid=NUS_RX_UUID; chr.val_handle=7; cb(h, &e, &chr, ctx);
    if(!incomplete_chars) {e.status=1; cb(h, &e, NULL, ctx);} return 0;
}
static int ble_gattc_disc_all_dscs(uint16_t h, uint16_t start, uint16_t end, dsc_cb cb, void* ctx) {
    (void)start; (void)end;
    struct ble_gatt_error e={0}; struct ble_gatt_dsc dsc={CCCD_UUID, 8};
    cb(h, &e, 7, &dsc, ctx); e.status=1; cb(h, &e, 7, NULL, ctx); return 0;
}
static int ble_sm_inject_io(uint16_t h, const struct ble_sm_io* io) {
    (void)h; injected_pin=io->passkey; return 0;
}
static int ble_gap_security_initiate(uint16_t h) {
    ++security_calls;
    struct ble_gap_event e={0}; e.type=BLE_GAP_EVENT_PASSKEY_ACTION;
    e.passkey.conn_handle=h; e.passkey.params.action=BLE_SM_IOACT_INPUT; cham_gap_cb(&e, NULL);
    e.type=BLE_GAP_EVENT_ENC_CHANGE; e.enc_change.status=fail_pairing ? 1 : 0;
    cham_gap_cb(&e, NULL); return 0;
}
static size_t make_response(uint8_t* p, uint16_t cmd, uint16_t status, size_t n) {
    p[0]=0x11; p[1]=0xef; p[2]=(uint8_t)(cmd>>8); p[3]=(uint8_t)cmd;
    p[4]=(uint8_t)(status>>8); p[5]=(uint8_t)status; p[6]=(uint8_t)(n>>8); p[7]=(uint8_t)n;
    p[8]=cham_lrc(p, 8);
    for(size_t i=0; i<n; ++i) p[9+i]=(uint8_t)i;
    p[9+n]=cham_lrc(p, 9+n); return 10+n;
}
static int ble_gattc_write_flat(uint16_t h, uint16_t handle, const void* p, uint16_t n, write_cb cb, void* ctx) {
    struct ble_gatt_error e={0};
    if(handle==8) {
        ++cccd_attempts;
        e.status=require_pin && security_calls==0 ? BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) : 0;
        cb(h, &e, NULL, ctx); return 0;
    }
    assert(n <= negotiated_mtu-3); ++writes;
    memcpy(tx+tx_len, p, n); tx_len+=n;
    if(disconnect_write) {ble_gap_terminate(h, 0); return 0;}
    e.status=fail_write ? 1 : 0; cb(h, &e, NULL, ctx);
    if(tx_len>=10 && tx_len==10u+(((uint16_t)tx[6]<<8)|tx[7])) {
        assert(cham_lrc(tx, tx_len)==0);
        rx_len=make_response(rx, ((uint16_t)tx[2]<<8)|tx[3], 0x68, 2); response_pending=true;
    }
    return 0;
}
static void reset_peer(void) {
    chameleon_disconnect();
    require_pin=fail_pairing=fail_write=disconnect_write=incomplete_chars=no_device=response_pending=false;
    tx_len=rx_len=0; writes=cccd_attempts=security_calls=0; injected_pin=0; pending_mtu=NULL;
    s_acc_len=0; s_resp_ready=false; s_expected_command=1000;
}
int main(void) {
    s_expected_command=1000;
    /* A corrupted reply must never be treated as a successful command. */
    rx_len=make_response(rx, 1000, 0x68, 2); rx[9]^=1;
    memcpy(s_acc, rx, rx_len); s_acc_len=rx_len; cham_try_parse(); assert(!s_resp_ready);
    rx_len=make_response(rx, 1000, 0x168, 2);
    for(size_t i=0; i<rx_len; ++i) {
        s_acc[s_acc_len++]=rx[i]; cham_try_parse();
        assert(s_resp_ready==(i==rx_len-1));
    }
    assert(s_last_resp.status==0x168 && s_last_resp.data_len==2);
    s_resp_ready=false; s_acc_len=make_response(s_acc, 1025, 0x68, 0);
    s_acc_len+=make_response(s_acc+s_acc_len, 1000, 0x68, 0);
    cham_try_parse(); assert(s_resp_ready && s_last_resp.command==1000 && s_acc_len==0);
    reset_peer(); assert(chameleon_connect(NULL) && chameleon_is_connected());
    uint8_t payload[512]={0}; ChameleonResp r;
    assert(chameleon_cmd(1000, payload, sizeof(payload), &r, 100));
    assert(writes==27 && tx_len==522 && r.command==1000);
    reset_peer(); require_pin=true;
    assert(chameleon_connect(NULL) && security_calls==1 && injected_pin==123456 && cccd_attempts==2);
    reset_peer(); require_pin=fail_pairing=true;
    int before=restores;
    assert(!chameleon_connect(NULL) && !chameleon_is_connected() && restores==before+1);
    reset_peer(); incomplete_chars=true; assert(!chameleon_connect(NULL));
    reset_peer(); no_device=true; assert(!chameleon_connect(NULL));
    reset_peer(); volatile bool abort=true; assert(!chameleon_connect(&abort));
    reset_peer(); assert(chameleon_connect(NULL)); fail_write=true;
    assert(!chameleon_cmd(1000, NULL, 0, &r, 100));
    reset_peer(); assert(chameleon_connect(NULL)); disconnect_write=true;
    assert(!chameleon_cmd(1000, NULL, 0, &r, 100) && !chameleon_is_connected());
    reset_peer(); bt_enabled=false; before=restores;
    assert(chameleon_connect(NULL)); chameleon_disconnect(); assert(restores==before);
    puts("PASS: Chameleon checksums, fragments, stale replies, MTU chunks, PIN pairing, discovery/abort/write failure cleanup");
    return 0;
}
'''
out = root / 'build_host/chameleon'
out.mkdir(parents=True, exist_ok=True)
test = out / 'test.c'
test.write_text(fixture, encoding='utf-8')
exe = out / 'test.exe'
subprocess.run(['cl', '/nologo', '/std:c11', '/utf-8', str(test),
                '/Fo' + str(out) + '/', '/Fe' + str(exe)], check=True)
subprocess.run([str(exe)], check=True)
