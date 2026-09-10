#pragma once

#include <gui/view.h>
#include <stdint.h>

typedef struct {
    char status[24];
    uint32_t speed_kbps;
    uint32_t bytes_done;
    uint32_t bytes_total;
    uint8_t percent;
} WlanFwUpdateViewModel;

View* wlan_fw_update_view_alloc(void);
void wlan_fw_update_view_free(View* view);

void wlan_fw_update_view_update(
    View* view,
    const char* status,
    uint8_t percent,
    uint32_t bytes_done,
    uint32_t bytes_total,
    uint32_t speed_kbps);
