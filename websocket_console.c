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
#define WS_TX_BUFFER_SIZE 1024

static queue_t ws_rx_queue;
static uint8_t ws_tx_buffer[WS_TX_BUFFER_SIZE];
static size_t ws_tx_head = 0;
static size_t ws_tx_tail = 0;
static mutex_t ws_tx_lock;

static void websocket_console_clear_tx_buffer(void);
static void websocket_console_clear_queues(void);

static inline size_t websocket_console_tx_advance(size_t index, size_t count)
{
    index += count;
    if (index >= WS_TX_BUFFER_SIZE)
    {
        index -= WS_TX_BUFFER_SIZE;
    }
    return index;
}

void websocket_console_tx_init(void)
{
    mutex_enter_blocking(&ws_tx_lock);
    ws_tx_head = 0;
    ws_tx_tail = 0;
    mutex_exit(&ws_tx_lock);
}

static void websocket_console_tx_push(uint8_t value)
{
    mutex_enter_blocking(&ws_tx_lock);

    size_t next_head = websocket_console_tx_advance(ws_tx_head, 1);
    if (next_head == ws_tx_tail)
    {
        // Buffer full, drop oldest byte to make room
        ws_tx_tail = websocket_console_tx_advance(ws_tx_tail, 1);
    }

    ws_tx_buffer[ws_tx_head] = value;
    ws_tx_head = next_head;

    mutex_exit(&ws_tx_lock);
}

static size_t websocket_console_tx_pop(uint8_t *buffer, size_t max_len)
{
    if (!buffer || max_len == 0)
    {
        return 0;
    }

    mutex_enter_blocking(&ws_tx_lock);

    size_t total_copied = 0;
    while (total_copied < max_len && ws_tx_tail != ws_tx_head)
    {
        size_t contiguous = (ws_tx_head > ws_tx_tail)
                                ? (ws_tx_head - ws_tx_tail)
                                : (WS_TX_BUFFER_SIZE - ws_tx_tail);
        size_t to_copy = max_len - total_copied;
        if (to_copy > contiguous)
        {
            to_copy = contiguous;
        }

        memcpy(&buffer[total_copied], &ws_tx_buffer[ws_tx_tail], to_copy);
        total_copied += to_copy;
        ws_tx_tail = websocket_console_tx_advance(ws_tx_tail, to_copy);
    }

    mutex_exit(&ws_tx_lock);
    return total_copied;
}

static void websocket_console_clear_tx_buffer(void)
{
    mutex_enter_blocking(&ws_tx_lock);
    ws_tx_head = 0;
    ws_tx_tail = 0;
    mutex_exit(&ws_tx_lock);
}

void websocket_queue_init(void)
{

    // Initialize mutex and queues on core 0 before launching core 1
    mutex_init(&ws_tx_lock);
    queue_init(&ws_rx_queue, sizeof(uint8_t), WS_RX_QUEUE_DEPTH);
}

void websocket_console_enqueue_output(uint8_t value)
{
    if (!ws_has_active_clients())
    {
        websocket_console_clear_tx_buffer();
        return;
    }

    websocket_console_tx_push(value);
}

bool websocket_console_try_dequeue_input(uint8_t *value)
{
    return queue_try_remove(&ws_rx_queue, value);
}

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

void websocket_console_on_client_connected(void *user_data)
{
    (void)user_data;
}

void websocket_console_on_client_disconnected(void *user_data)
{
    (void)user_data;
    websocket_console_clear_queues();
}

static void websocket_console_clear_queues(void)
{
    websocket_console_clear_tx_buffer();

    uint8_t discard = 0;
    while (queue_try_remove(&ws_rx_queue, &discard))
    {
    }
}

size_t websocket_console_supply_output(uint8_t *buffer, size_t max_len, void *user_data)
{
    (void)user_data;
    return websocket_console_tx_pop(buffer, max_len);
}

#else // No WiFi capability

void websocket_console_enqueue_output(uint8_t value)
{
    (void)value;
}

bool websocket_console_try_dequeue_input(uint8_t *value)
{
    (void)value;
    return false;
}

#endif // No WiFi capability