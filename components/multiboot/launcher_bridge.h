#pragma once
#include <stdbool.h>
#include <esp_err.h>

/* Only the companion Launcher factory image registered in NVS is trusted. */
bool launcher_bridge_is_hosted(void);
/* Runs flash/NVS work on an internal-RAM stack; restarts only on success. */
esp_err_t launcher_bridge_return(void);
