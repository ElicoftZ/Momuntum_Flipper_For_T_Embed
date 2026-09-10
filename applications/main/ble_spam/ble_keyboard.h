#pragma once

#include "ble_spam_app.h"

bool ble_keyboard_start(BleSpamApp* app);
void ble_keyboard_stop(BleSpamApp* app);
bool ble_keyboard_send_key(BleSpamApp* app, uint16_t keycode);
bool ble_keyboard_send_consumer_key(BleSpamApp* app, uint16_t keycode);
bool ble_keyboard_mouse_move(BleSpamApp* app, int8_t dx, int8_t dy);
bool ble_keyboard_mouse_click(BleSpamApp* app, uint8_t button);
bool ble_keyboard_send_text(BleSpamApp* app, const char* text, bool append_enter);
void ble_keyboard_remove_pairing(BleSpamApp* app);
