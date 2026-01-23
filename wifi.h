#pragma once

#include <stdbool.h>
#include <stddef.h>

bool wifi_is_ready(void);
bool wifi_is_connected(void);
bool wifi_get_ip(char* buffer, size_t length);

// Set WiFi state (should be called by core1_io_mgr)
void wifi_set_ready(bool ready);
void wifi_set_connected(bool connected);

// Cached IP address access (simple, no lwIP calls)
void wifi_set_ip_address(const char* ip);
const char* wifi_get_ip_address(void); // Returns NULL if not set
