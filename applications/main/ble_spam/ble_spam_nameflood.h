#pragma once

#include <stddef.h>

void ble_spam_nameflood_reload(void);
size_t ble_spam_nameflood_name_count(void);
const char* ble_spam_nameflood_name_at(size_t index);
