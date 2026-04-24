/**
 * @file websocket_console.h
 * @brief WebSocket console for Altair 8800 terminal I/O
 *
 * Provides cross-core communication between the WebSocket server (Core 1)
 * and the Altair emulator (Core 0) using Pico SDK queues.
 * Single-client model: only one WebSocket client at a time.
 * New connections automatically kick existing clients.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Launch core1 which initializes Wi-Fi, starts the WebSocket server, and runs the poll loop.
// Call this once from main(); it returns immediately while core1 runs in the background.
void websocket_console_start(void);

// Enqueue a byte from the emulator (core 0) to be sent to WebSocket clients.
// 4KB TX queue is large enough for both normal terminal and CPU monitor bulk output.
void websocket_console_enqueue_output(uint8_t value);

// Try to dequeue a byte received from WebSocket clients (called from core 0).
bool websocket_console_try_dequeue_input(uint8_t* value);

// Check whether a WebSocket client is currently connected.
bool websocket_console_has_client(void);

// Try to dequeue a byte received from the CPU monitor input queue (called from core 0).
bool websocket_console_try_dequeue_monitor_input(uint8_t* value);

// Enqueue a byte to the CPU monitor input queue (called from core 1).
void websocket_console_enqueue_monitor_input(uint8_t value);

// Enqueue a byte to the terminal input queue (called from core 1, e.g. BT keyboard).
void websocket_console_enqueue_input(uint8_t value);

// Route a single input byte: handle Ctrl+M toggle, \n→\r, and
// dispatch to the correct queue based on CPU mode.
// Used by both WebSocket and BT keyboard input paths.
void websocket_console_route_input(uint8_t ch);

// Check if the console is running and Wi-Fi is connected.
bool websocket_console_is_running(void);

// Forward declarations for internal functions
void ws_poll_incoming(void);
void ws_poll_outgoing(void);

// Poll the WebSocket server for incoming and outgoing messages (internal use)
static inline void ws_poll(volatile bool* pending_ws_input, volatile bool* pending_ws_output)
{
    if (*pending_ws_input)
    {
        *pending_ws_input = false;
        ws_poll_incoming();
    }
    if (*pending_ws_output)
    {
        *pending_ws_output = false;
        ws_poll_outgoing();
    }
}

// Initialize WebSocket queues (internal use)
void websocket_queue_init(void);

// Initialize and start the WebSocket server (returns true on success)
bool websocket_console_init_server(void);

// Callback from main.c to handle client connection
void client_connected_cb(void);

// Forward declaration for CPU monitor function (caller buffer is not mutated)
void process_virtual_input(const char* command, size_t len);

// WebSocket callback functions (internal use)
bool websocket_console_handle_input(const uint8_t* payload, size_t payload_len, void* user_data);
size_t websocket_console_supply_output(uint8_t* buffer, size_t max_len, void* user_data);
void websocket_console_on_client_connected(void* user_data);
void websocket_console_on_client_disconnected(void* user_data);

#ifdef VT100_DISPLAY
bool vt100_try_dequeue_output(uint8_t* value);
#endif
