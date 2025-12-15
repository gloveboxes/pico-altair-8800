#include "websocket_console.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "pico/multicore.h"
#include "pico/mutex.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"

#include "ws.h"

// Enable WebSocket console only if board has WiFi capability
#if defined(CYW43_WL_GPIO_LED_PIN)

#define WS_RX_QUEUE_DEPTH 128
#define WS_TX_QUEUE_DEPTH 512

static queue_t ws_rx_queue;
static queue_t ws_tx_queue;

static void websocket_console_clear_tx_buffer(void);
static void websocket_console_clear_queues(void);

/**
 * @brief Pushes a byte into the WebSocket console transmit buffer.
 * 
 * Adds a byte to the TX queue. If the queue is full,
 * the oldest byte is discarded to make room for the new one.
 * Thread-safe, uses Pico SDK queue.
 * 
 * @param value Byte to add to the transmit buffer
 */
static void websocket_console_tx_push(uint8_t value)
{
    if (!queue_try_add(&ws_tx_queue, &value))
    {
        // Queue full, drop oldest byte to make room
        uint8_t discard = 0;
        if (queue_try_remove(&ws_tx_queue, &discard))
        {
            queue_try_add(&ws_tx_queue, &value);
        }
    }
}

/**
 * @brief Pops bytes from the WebSocket console transmit buffer.
 * 
 * Retrieves up to max_len bytes from the TX queue.
 * Thread-safe, uses Pico SDK queue.
 * 
 * @param buffer Destination buffer for retrieved bytes
 * @param max_len Maximum number of bytes to retrieve
 * @return size_t Number of bytes actually retrieved
 */
static size_t websocket_console_tx_pop(uint8_t *buffer, size_t max_len)
{
    if (!buffer || max_len == 0)
    {
        return 0;
    }

    size_t count = 0;
    while (count < max_len && queue_try_remove(&ws_tx_queue, &buffer[count]))
    {
        count++;
    }

    return count;
}

/**
 * @brief Clears the WebSocket console transmit buffer.
 * 
 * Removes all pending bytes from the TX queue.
 */
static void websocket_console_clear_tx_buffer(void)
{
    uint8_t discard = 0;
    while (queue_try_remove(&ws_tx_queue, &discard))
    {
    }
}

/**
 * @brief Initializes the WebSocket console queues and synchronization primitives.
 * 
 * Sets up the TX and RX queues. Must be called on core 0
 * before launching core 1 to ensure proper multi-core synchronization.
 */
void websocket_queue_init(void)
{
    // Initialize queues on core 0 before launching core 1
    queue_init(&ws_tx_queue, sizeof(uint8_t), WS_TX_QUEUE_DEPTH);
    queue_init(&ws_rx_queue, sizeof(uint8_t), WS_RX_QUEUE_DEPTH);
}

/**
 * @brief Initializes and starts the WebSocket server.
 * 
 * Sets up WebSocket callbacks and starts the server.
 * Should be called after WiFi is connected.
 * 
 * @return true if server started successfully, false otherwise
 */
bool websocket_console_init_server(void)
{
    ws_callbacks_t callbacks = {
        .on_receive = websocket_console_handle_input,
        .on_output = websocket_console_supply_output,
        .on_client_connected = websocket_console_on_client_connected,
        .on_client_disconnected = websocket_console_on_client_disconnected,
        .user_data = NULL,
    };
    ws_init(&callbacks);

    return ws_start();
}

/**
 * @brief Enqueues a byte for transmission to WebSocket clients.
 * 
 * Adds a byte to the TX buffer for sending to connected WebSocket clients.
 * If no clients are connected, clears the buffer instead to prevent accumulation.
 * 
 * @param value Byte to transmit to WebSocket clients
 */
void websocket_console_enqueue_output(uint8_t value)
{
    if (!ws_has_active_clients())
    {
        websocket_console_clear_tx_buffer();
        return;
    }

    websocket_console_tx_push(value);
}

/**
 * @brief Attempts to dequeue a byte from the WebSocket input buffer.
 * 
 * Non-blocking attempt to retrieve a byte received from WebSocket clients.
 * 
 * @param value Pointer to store the retrieved byte
 * @return true if a byte was successfully retrieved, false if queue is empty
 */
bool websocket_console_try_dequeue_input(uint8_t *value)
{
    return queue_try_remove(&ws_rx_queue, value);
}

/**
 * @brief Handles incoming WebSocket input data.
 * 
 * Processes bytes received from WebSocket clients and adds them to the RX queue.
 * Converts newline characters (\n) to carriage returns (\r).
 * If the queue is full, discards the oldest byte to make room.
 * 
 * @param payload Pointer to incoming data bytes
 * @param payload_len Number of bytes in the payload
 * @param user_data User-defined context (unused)
 * @return true if processing succeeded, false if payload is NULL
 */
bool websocket_console_handle_input(const uint8_t *payload, size_t payload_len, void *user_data)
{
    (void)user_data;

    if (!payload)
    {
        return false;
    }

    for (size_t i = 0; i < payload_len; ++i)
    {
        uint8_t ch = payload[i];
        if (ch == '\n')
        {
            ch = '\r';
        }
        if (!queue_try_add(&ws_rx_queue, &ch))
        {
            uint8_t discard = 0;
            if (queue_try_remove(&ws_rx_queue, &discard))
            {
                queue_try_add(&ws_rx_queue, &ch);
            }
        }
    }

    return true;
}

/**
 * @brief Callback invoked when a WebSocket client connects.
 * 
 * Calls the client_connected_cb function to update CPU mode.
 * 
 * @param user_data User-defined context (unused)
 */
void websocket_console_on_client_connected(void *user_data)
{
    (void)user_data;
    client_connected_cb();
}

/**
 * @brief Callback invoked when a WebSocket client disconnects.
 * 
 * Clears both TX and RX queues to reset console state when
 * the client connection is lost.
 * 
 * @param user_data User-defined context (unused)
 */
void websocket_console_on_client_disconnected(void *user_data)
{
    (void)user_data;
    websocket_console_clear_queues();
}

/**
 * @brief Clears both TX and RX queues.
 * 
 * Empties the transmit buffer and removes all pending bytes
 * from the receive queue.
 */
static void websocket_console_clear_queues(void)
{
    websocket_console_clear_tx_buffer();

    uint8_t discard = 0;
    while (queue_try_remove(&ws_rx_queue, &discard))
    {
    }
}

/**
 * @brief Supplies output data to be sent to WebSocket clients.
 * 
 * Called by the WebSocket server to retrieve bytes from the TX buffer
 * for transmission to connected clients.
 * 
 * @param buffer Destination buffer for output data
 * @param max_len Maximum number of bytes to retrieve
 * @param user_data User-defined context (unused)
 * @return size_t Number of bytes placed in the buffer
 */
size_t websocket_console_supply_output(uint8_t *buffer, size_t max_len, void *user_data)
{
    (void)user_data;
    return websocket_console_tx_pop(buffer, max_len);
}

#else // No WiFi capability

/**
 * @brief Stub for initializing WebSocket server when WiFi is not available.
 * 
 * @return false Always returns false (no WiFi capability)
 */
bool websocket_console_init_server(void)
{
    return false;
}

/**
 * @brief Stub for enqueuing output when WiFi is not available.
 * 
 * @param value Unused byte value
 */
void websocket_console_enqueue_output(uint8_t value)
{
    (void)value;
}

/**
 * @brief Stub for dequeuing input when WiFi is not available.
 * 
 * @param value Unused pointer
 * @return false Always returns false (no input available)
 */
bool websocket_console_try_dequeue_input(uint8_t *value)
{
    (void)value;
    return false;
}

#endif // No WiFi capability