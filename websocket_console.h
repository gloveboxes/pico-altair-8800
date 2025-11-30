#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Launch core1 which initializes Wi-Fi, starts the WebSocket server, and runs the poll loop.
// Call this once from main(); it returns immediately while core1 runs in the background.
void websocket_console_start(void);

// Block until core 1 completes Wi-Fi initialization.
// Returns 0 on failure, or raw 32-bit IP address on success.
uint32_t websocket_console_wait_for_wifi(void);

// Enqueue a byte from the emulator (core 0) to be sent to WebSocket clients.
void websocket_console_enqueue_output(uint8_t value);

// Try to dequeue a byte received from WebSocket clients (called from core 0).
bool websocket_console_try_dequeue_input(uint8_t *value);

// Check if the console is running and Wi-Fi is connected.
bool websocket_console_is_running(void);

// Get the IP address string (valid after Wi-Fi connects). Returns false if not ready.
bool websocket_console_get_ip(char *buffer, size_t length);
