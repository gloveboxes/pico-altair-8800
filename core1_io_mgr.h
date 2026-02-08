#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "websocket_console.h"

void websocket_console_start(void);
uint32_t wait_for_wifi(void);
const char* get_connected_ssid(void);

// Returns true if running in AP mode (captive portal)
bool is_ap_mode_active(void);