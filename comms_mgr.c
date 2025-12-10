#include "comms_mgr.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "websocket_console.h"

// Enable WiFi/WebSocket functionality only if board has WiFi capability
#if defined(CYW43_WL_GPIO_LED_PIN)

#include "cyw43.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "wifi_config.h"
#include "ws.h"

#ifndef WIFI_AUTH
#define WIFI_AUTH CYW43_AUTH_WPA2_AES_PSK
#endif

#define WIFI_CONNECT_TIMEOUT_MS 30000

static void websocket_console_core1_entry(void);

volatile bool console_running = false;
volatile bool console_initialized = false;
volatile bool wifi_connected = false;

char ip_address_buffer[32] = {0};

static bool websocket_console_wifi_init(void)
{
    printf("[Core1] Initializing CYW43...\n");
    if (cyw43_arch_init())
    {
        printf("[Core1] CYW43 init failed\n");
        return false;
    }

    cyw43_arch_enable_sta_mode();

    // Load credentials from flash storage
    char ssid[WIFI_CONFIG_SSID_MAX_LEN + 1] = {0};
    char password[WIFI_CONFIG_PASSWORD_MAX_LEN + 1] = {0};
    bool credentials_loaded = wifi_config_load(ssid, sizeof(ssid), password, sizeof(password));

    // Check if credentials were loaded successfully
    if (!credentials_loaded || ssid[0] == '\0')
    {
        printf("[Core1] No WiFi credentials configured, Wi-Fi unavailable\n");
        return false;
    }

    printf("[Core1] Using stored credentials from flash\n");

    printf("[Core1] Connecting to Wi-Fi SSID '%s'...\n", ssid);

    int err = cyw43_arch_wifi_connect_timeout_ms(
        ssid,
        password,
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

void websocket_console_start(void)
{
    if (console_running)
    {
        return;
    }

    websocket_queue_init();

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

    // Mark console as initialized only after successful network stack initialization
    console_initialized = true;
    printf("[Core1] WebSocket server running, entering poll loop\n");

    // Main poll loop - all CYW43/lwIP access stays on core 1
    int counter = 0;
    while (true)
    {
        cyw43_arch_poll();
        ws_poll(&counter);
        tight_loop_contents();
    }
}

#else // No WiFi capability

void websocket_console_start(void)
{
    printf("WebSocket console disabled; USB serial only.\n");
}

uint32_t websocket_console_wait_for_wifi(void)
{
    return 0;
}

bool websocket_console_is_running(void)
{
    return false;
}

bool websocket_console_get_ip(char *buffer, size_t length)
{
    (void)buffer;
    (void)length;
    return false;
}

#endif // CYW43_WL_GPIO_LED_PIN