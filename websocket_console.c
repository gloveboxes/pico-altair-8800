#include "websocket_console.h"

#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "pico/sync.h"

#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "cyw43.h"

#include "ws.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef WIFI_SSID
#define WIFI_SSID ""
#endif

#ifndef WIFI_PASSWORD
#define WIFI_PASSWORD ""
#endif

#ifndef WIFI_AUTH
#define WIFI_AUTH CYW43_AUTH_WPA2_AES_PSK
#endif

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WS_RX_QUEUE_DEPTH 128
#define WS_TX_BUFFER_SIZE 1024

static queue_t ws_rx_queue;
static uint8_t ws_tx_buffer[WS_TX_BUFFER_SIZE];
static size_t ws_tx_head = 0;
static size_t ws_tx_tail = 0;
static critical_section_t ws_tx_lock;
static bool ws_tx_lock_initialized = false;
static volatile bool console_initialized = false;
static volatile bool console_running = false;
static volatile bool wifi_connected = false;
static char ip_address_buffer[32] = {0};

static void websocket_console_core1_entry(void);
static bool websocket_console_wifi_init(void);
static bool websocket_console_handle_input(const uint8_t *payload, size_t payload_len, void *user_data);
static size_t websocket_console_supply_output(uint8_t *buffer, size_t max_len, void *user_data);
static void websocket_console_on_client_connected(void *user_data);
static void websocket_console_on_client_disconnected(void *user_data);
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

static void websocket_console_tx_init(void)
{
    if (!ws_tx_lock_initialized)
    {
        critical_section_init(&ws_tx_lock);
        ws_tx_lock_initialized = true;
    }
    critical_section_enter_blocking(&ws_tx_lock);
    ws_tx_head = 0;
    ws_tx_tail = 0;
    critical_section_exit(&ws_tx_lock);
}

static void websocket_console_tx_push(uint8_t value)
{
    if (!ws_tx_lock_initialized)
    {
        websocket_console_tx_init();
    }

    critical_section_enter_blocking(&ws_tx_lock);

    size_t next_head = websocket_console_tx_advance(ws_tx_head, 1);
    if (next_head == ws_tx_tail)
    {
        // Buffer full, drop oldest byte to make room
        ws_tx_tail = websocket_console_tx_advance(ws_tx_tail, 1);
    }

    ws_tx_buffer[ws_tx_head] = value;
    ws_tx_head = next_head;

    critical_section_exit(&ws_tx_lock);
}

static size_t websocket_console_tx_pop(uint8_t *buffer, size_t max_len)
{
    if (!buffer || max_len == 0 || !ws_tx_lock_initialized)
    {
        return 0;
    }

    critical_section_enter_blocking(&ws_tx_lock);

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

    critical_section_exit(&ws_tx_lock);
    return total_copied;
}

static void websocket_console_clear_tx_buffer(void)
{
    if (!ws_tx_lock_initialized)
    {
        websocket_console_tx_init();
        return;
    }

    critical_section_enter_blocking(&ws_tx_lock);
    ws_tx_head = 0;
    ws_tx_tail = 0;
    critical_section_exit(&ws_tx_lock);
}

static void websocket_console_init(void)
{
    if (console_initialized)
    {
        return;
    }

    queue_init(&ws_rx_queue, sizeof(uint8_t), WS_RX_QUEUE_DEPTH);
    websocket_console_tx_init();

    ws_callbacks_t callbacks = {
        .on_receive = websocket_console_handle_input,
        .on_output = NULL,
        .on_client_connected = websocket_console_on_client_connected,
        .on_client_disconnected = websocket_console_on_client_disconnected,
        .user_data = NULL,
    };
    ws_init(&callbacks);

    console_initialized = true;
}

void websocket_console_start(void)
{
    if (console_running)
    {
        return;
    }

    // Initialize queues on core 0 before launching core 1
    queue_init(&ws_rx_queue, sizeof(uint8_t), WS_RX_QUEUE_DEPTH);
    websocket_console_tx_init();
    console_initialized = true;

    // Launch core 1 which will handle all Wi-Fi and WebSocket operations
    multicore_launch_core1(websocket_console_core1_entry);
    console_running = true;
    printf("Launched network task on core 1\n");
}

uint32_t websocket_console_wait_for_wifi(void)
{
    // Block until core 1 signals Wi-Fi init complete
    // Returns 0 on failure, or raw 32-bit IP address on success
    return multicore_fifo_pop_blocking();
}

bool websocket_console_is_running(void)
{
    return console_running && wifi_connected && ws_is_running();
}

bool websocket_console_get_ip(char *buffer, size_t length)
{
    if (!wifi_connected || !buffer || length == 0)
    {
        return false;
    }

    size_t ip_len = strlen(ip_address_buffer);
    if (ip_len == 0 || ip_len >= length)
    {
        return false;
    }

    strncpy(buffer, ip_address_buffer, length - 1);
    buffer[length - 1] = '\0';
    return true;
}

void websocket_console_enqueue_output(uint8_t value)
{
    if (!console_initialized)
    {
        return;
    }

    if (!ws_has_active_clients())
    {
        websocket_console_clear_tx_buffer();
        return;
    }

    websocket_console_tx_push(value);
}

bool websocket_console_try_dequeue_input(uint8_t *value)
{
    if (!console_initialized || !value)
    {
        return false;
    }

    return queue_try_remove(&ws_rx_queue, value);
}

static bool websocket_console_wifi_init(void)
{
    printf("[Core1] Initializing CYW43...\n");
    if (cyw43_arch_init())
    {
        printf("[Core1] CYW43 init failed\n");
        return false;
    }

    cyw43_arch_enable_sta_mode();
    printf("[Core1] Connecting to Wi-Fi SSID '%s'...\n", WIFI_SSID);

    int err = cyw43_arch_wifi_connect_timeout_ms(
        WIFI_SSID,
        WIFI_PASSWORD,
        WIFI_AUTH,
        WIFI_CONNECT_TIMEOUT_MS);

    if (err != 0)
    {
        printf("[Core1] Wi-Fi connect failed (err=%d)\n", err);
        return false;
    }

    // Get and store IP address
    struct netif *netif = netif_default;
    if (netif && netif_is_up(netif))
    {
        const ip4_addr_t *addr = netif_ip4_addr(netif);
        if (addr)
        {
            ip4addr_ntoa_r(addr, ip_address_buffer, sizeof(ip_address_buffer));
        }
    }

    printf("[Core1] Wi-Fi connected. IP: %s\n", ip_address_buffer);
    return true;
}

static void websocket_console_core1_entry(void)
{
    // Initialize Wi-Fi on core 1
    bool wifi_ok = websocket_console_wifi_init();
    wifi_connected = wifi_ok;

    // Signal core 0: send 0 for failure, or raw IP address (32-bit) for success
    uint32_t ip_raw = 0;
    if (wifi_ok)
    {
        struct netif *netif = netif_default;
        if (netif && netif_is_up(netif))
        {
            const ip4_addr_t *addr = netif_ip4_addr(netif);
            if (addr)
            {
                ip_raw = ip4_addr_get_u32(addr);
            }
        }
    }
    multicore_fifo_push_blocking(ip_raw);

    if (!wifi_ok)
    {
        printf("[Core1] Wi-Fi unavailable, network task exiting\n");
        return;
    }

    // Initialize and start WebSocket server
    ws_callbacks_t callbacks = {
        .on_receive = websocket_console_handle_input,
        .on_output = websocket_console_supply_output,
        .on_client_connected = websocket_console_on_client_connected,
        .on_client_disconnected = websocket_console_on_client_disconnected,
        .user_data = NULL,
    };
    ws_init(&callbacks);

    if (!ws_start())
    {
        printf("[Core1] Failed to start WebSocket server\n");
        return;
    }

    printf("[Core1] WebSocket server running, entering poll loop\n");

    // Main poll loop - all CYW43/lwIP access stays on core 1
    while (true)
    {
        cyw43_arch_poll();
        ws_poll();
        tight_loop_contents();
    }
}

static bool websocket_console_handle_input(const uint8_t *payload, size_t payload_len, void *user_data)
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

static void websocket_console_on_client_connected(void *user_data)
{
    (void)user_data;
}

static void websocket_console_on_client_disconnected(void *user_data)
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

static size_t websocket_console_supply_output(uint8_t *buffer, size_t max_len, void *user_data)
{
    (void)user_data;
    return websocket_console_tx_pop(buffer, max_len);
}