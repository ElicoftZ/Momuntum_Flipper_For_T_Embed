"""Exercise production NFC source switching with simulated PN532 handshakes.

Run from an MSVC developer shell.
"""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
source = (root / 'components/furi_hal/furi_hal_nfc.c').read_text(encoding='utf-8')


def function(signature):
    start = source.index(signature)
    end = source.index('{', start) + 1
    depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


fixture = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define BOARD_PIN_NFC_SDA 8
#define BOARD_PIN_NFC_SCL 18
#define BOARD_NFC_I2C_PORT 0
#define I2C_NUM_1 1
#define BOARD_PIN_NFC_IRQ 17
#define PN532_I2C_ADDR 0x24
#define ESP_OK 0
#define pdMS_TO_TICKS(x) (x)
typedef int esp_err_t;
#define FURI_LOG_W(...) ((void)0)
typedef int i2c_port_t;
enum {FuriHalNfcErrorNone, FuriHalNfcErrorCommunication};
static bool nfc_hal_ready = true;
static int pn532_target_number = 1;
static int nfc_mutex_storage;
static int* nfc_mutex = &nfc_mutex_storage;
enum {FuriStatusOk, FuriWaitForever = -1};
static int locked, lock_failure;
static int furi_mutex_acquire(int* mutex, int timeout) {
    (void)mutex; (void)timeout;
    if(lock_failure) return 1;
    assert(!locked); locked = 1; return FuriStatusOk;
}
static int furi_mutex_release(int* mutex) {
    (void)mutex; assert(locked); locked = 0; return FuriStatusOk;
}
'''
fixture += source[source.index('enum {\n    BOARD_PIN_NFC_SDA_DEFAULT'):source.index('static esp_err_t pn532_i2c_init')]
fixture += r'''
static uint32_t tick;
static int irq_reads, i2c_reads;
static bool irq_low, external_ready;
static uint32_t furi_get_tick(void) {return tick;}
static void furi_delay_ms(int ms) {tick += ms;}
static int gpio_get_level(int pin) {assert(pin == 17); ++irq_reads; return !irq_low;}
static int i2c_master_read_from_device(int port, int addr, uint8_t* p, size_t n, int timeout) {
    (void)timeout; assert(port == 1 && addr == 0x24 && n == 1);
    ++i2c_reads; *p = external_ready; return ESP_OK;
}
'''
fixture += function('static bool pn532_wait_ready(')
fixture += r'''
static bool onboard_present = true, external_present = true;
static int pn532_handshake(void) {
    if(nfc_i2c_port == 0) {
        assert(nfc_sda_gpio == 8 && nfc_scl_gpio == 18);
        return onboard_present ? FuriHalNfcErrorNone : FuriHalNfcErrorCommunication;
    }
    assert(nfc_i2c_port == 1 && nfc_sda_gpio == 43 && nfc_scl_gpio == 44);
    return external_present ? FuriHalNfcErrorNone : FuriHalNfcErrorCommunication;
}
'''
fixture += function('bool furi_hal_nfc_set_use_qwiic(')
fixture += function('bool furi_hal_nfc_is_using_qwiic(')
fixture += r'''
int main(void) {
    assert(furi_hal_nfc_set_use_qwiic(true) && furi_hal_nfc_is_using_qwiic());
    assert(nfc_hal_ready && pn532_target_number == 0);
    external_ready=true;
    assert(pn532_wait_ready(10) && irq_reads==0 && i2c_reads==1);
    external_ready=false; irq_low=true;
    assert(!pn532_wait_ready(10) && irq_reads==0); /* Onboard IRQ cannot ready an external reader. */
    assert(furi_hal_nfc_set_use_qwiic(false));
    assert(!furi_hal_nfc_is_using_qwiic() && nfc_i2c_port == 0);
    assert(pn532_wait_ready(10) && irq_reads==1);
    external_present = false;
    assert(!furi_hal_nfc_set_use_qwiic(true));
    assert(!nfc_using_qwiic && nfc_i2c_port == 0 && nfc_hal_ready);
    onboard_present = false;
    assert(!furi_hal_nfc_set_use_qwiic(true) && !nfc_hal_ready);
    external_present = true;
    assert(furi_hal_nfc_set_use_qwiic(true) && nfc_hal_ready);
    assert(!furi_hal_nfc_set_use_qwiic(false));
    assert(nfc_using_qwiic && nfc_i2c_port == 1 && nfc_hal_ready);
    lock_failure = 1;
    assert(!furi_hal_nfc_set_use_qwiic(false) && nfc_using_qwiic);
    assert(!locked);
    puts("PASS: NFC source round trip, missing module rollback, failed rollback, recovery, busy bus");
}
'''
out = root / 'build_host/nfc_source'
out.mkdir(parents=True, exist_ok=True)
test = out / 'test.c'
test.write_text(fixture, encoding='utf-8')
exe = out / 'test.exe'
subprocess.run(['cl', '/nologo', '/std:c11', '/utf-8', str(test),
                '/Fo' + str(out) + '/', '/Fe' + str(exe)], check=True)
subprocess.run([str(exe)], check=True)
