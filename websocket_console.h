#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Launch core1 which initializes Wi-Fi, starts the WebSocket server, and runs the poll loop.
// Call this once from main(); it returns immediately while core1 runs in the background.
void websocket_console_start(void);

// Enqueue a byte from the emulator (core 0) to be sent to WebSocket clients.
void websocket_console_enqueue_output(uint8_t value);

// Try to dequeue a byte received from WebSocket clients (called from core 0).
bool websocket_console_try_dequeue_input(uint8_t *value);

// Check if the console is running and Wi-Fi is connected.
bool websocket_console_is_running(void);

// Get the IP address string (valid after Wi-Fi connects). Returns false if not ready.
bool websocket_console_get_ip(char *buffer, size_t length);

// Forward declarations for internal functions
void ws_poll_incoming(void);
void ws_poll_outgoing(void);

// Poll the WebSocket server for incoming and outgoing messages (internal use)
static inline void ws_poll(int *counter)
{
    ws_poll_incoming();
    if (++(*counter) >= 2000)
    {
        *counter = 0;
        ws_poll_outgoing();
    }
}

// Initialize WebSocket queues (internal use)
void websocket_queue_init(void);

// WebSocket callback functions (internal use)
bool websocket_console_handle_input(const uint8_t *payload, size_t payload_len, void *user_data);
size_t websocket_console_supply_output(uint8_t *buffer, size_t max_len, void *user_data);
void websocket_console_on_client_connected(void *user_data);
void websocket_console_on_client_disconnected(void *user_data);
