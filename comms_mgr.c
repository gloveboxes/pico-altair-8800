#include "comms_mgr.h"

#include <stdio.h>
#include <string.h>

#include "hardware/timer.h"
#include "pico/stdlib.h"

#include "PortDrivers/http_io.h"
#include "websocket_console.h"

// Enable WiFi/WebSocket functionality only if board has WiFi capability
#if defined(CYW43_WL_GPIO_LED_PIN)

#include "cyw43.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "wifi.h"
#include "wifi_config.h"
#include "ws.h"

#if defined(REMOTE_FS_SUPPORT)
#include "Altair8800/remote_fs.h"
#endif

#ifndef WIFI_AUTH
#define WIFI_AUTH CYW43_AUTH_WPA2_AES_PSK
#endif

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WS_OUTPUT_TIMER_INTERVAL_MS 20
#define WS_INPUT_TIMER_INTERVAL_MS 5

static void websocket_console_core1_entry(void);

volatile bool console_running = false;
volatile bool console_initialized = false;
volatile bool wifi_connected = false;
volatile bool pending_ws_output = false;
volatile bool pending_ws_input = false;

char ip_address_buffer[32] = {0};
static char connected_ssid[WIFI_CONFIG_SSID_MAX_LEN + 1] = {0};

// Timer for periodic WebSocket output
static struct repeating_timer ws_output_timer;

// Timer for periodic WebSocket input
static struct repeating_timer ws_input_timer;

// Timer callback for output - fires every 20ms
static bool ws_output_timer_callback(struct repeating_timer* t)
{
    (void)t;
    pending_ws_output = true;
    return true; // Keep repeating
}

// Timer callback for input - fires every 10ms
static bool ws_input_timer_callback(struct repeating_timer* t)
{
    (void)t;
    pending_ws_input = true;
    return true; // Keep repeating
}

static bool wifi_init(void)
{
    printf("[Core1] Initializing CYW43...\n");
    if (cyw43_arch_init())
    {
        printf("[Core1] CYW43 init failed\n");
        wifi_set_ready(false);
        return false;
    }

    wifi_set_ready(true);
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

    // Store the SSID we're connecting to
    strncpy(connected_ssid, ssid, sizeof(connected_ssid) - 1);
    connected_ssid[sizeof(connected_ssid) - 1] = '\0';

    int err = cyw43_arch_wifi_connect_timeout_ms(ssid, password, WIFI_AUTH, WIFI_CONNECT_TIMEOUT_MS);

    if (err != 0)
    {
        printf("[Core1] Wi-Fi connect failed (err=%d)\n", err);
        wifi_set_connected(false);
        return false;
    }

    wifi_set_connected(true);

    // Use performance power management mode for better responsiveness
    // CYW43_PERFORMANCE_PM = short 200ms sleep retention, good balance
    cyw43_wifi_pm(&cyw43_state, CYW43_PERFORMANCE_PM);

    // Get and store IP address
    struct netif* netif = netif_default;
    if (netif && netif_is_up(netif))
    {
        const ip4_addr_t* addr = netif_ip4_addr(netif);
        if (addr)
        {
            ip4addr_ntoa_r(addr, ip_address_buffer, sizeof(ip_address_buffer));
            wifi_set_ip_address(ip_address_buffer); // Cache for display
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
    http_io_init(); // Initialize HTTP file transfer queues

    // Launch core 1 which will handle all Wi-Fi and WebSocket operations
    multicore_launch_core1(websocket_console_core1_entry);
    console_running = true;
    printf("Launched network task on core 1\n");
}

uint32_t wait_for_wifi(void)
{
    // Block until core 1 signals Wi-Fi init complete
    // Returns 0 on failure, or raw 32-bit IP address on success
    return multicore_fifo_pop_blocking();
}

const char* get_connected_ssid(void)
{
    return connected_ssid[0] != '\0' ? connected_ssid : NULL;
}

bool websocket_console_is_running(void)
{
    return console_running && wifi_connected && ws_is_running();
}

static void websocket_console_core1_entry(void)
{
    // Initialize Wi-Fi on core 1
    bool wifi_ok = wifi_init();
    wifi_connected = wifi_ok;

    // Signal core 0: send 0 for failure, or raw IP address (32-bit) for success
    uint32_t ip_raw = 0;
    if (wifi_ok)
    {
        struct netif* netif = netif_default;
        if (netif && netif_is_up(netif))
        {
            const ip4_addr_t* addr = netif_ip4_addr(netif);
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
    if (!websocket_console_init_server())
    {
        printf("[Core1] Failed to start WebSocket server\n");
        return;
    }

    // Start WebSocket timers on Core 1 (after WiFi init)
    add_repeating_timer_ms(-WS_OUTPUT_TIMER_INTERVAL_MS, ws_output_timer_callback, NULL, &ws_output_timer);
    printf("[Core1] Started WebSocket output timer (%dms interval)\n", WS_OUTPUT_TIMER_INTERVAL_MS);

    add_repeating_timer_ms(-WS_INPUT_TIMER_INTERVAL_MS, ws_input_timer_callback, NULL, &ws_input_timer);
    printf("[Core1] Started WebSocket input timer (%dms interval)\n", WS_INPUT_TIMER_INTERVAL_MS);

    // Mark console as initialized only after successful network stack initialization
    console_initialized = true;
    printf("[Core1] WebSocket server running, entering poll loop\n");

    // Main poll loop - all CYW43/lwIP access stays on core 1
    while (true)
    {
        cyw43_arch_poll();
        ws_poll(&pending_ws_input, &pending_ws_output);
        http_poll(); // Poll for HTTP file transfer requests
#if defined(REMOTE_FS_SUPPORT)
        rfs_client_poll(); // Poll remote FS client
#endif
        tight_loop_contents();
    }
}

#else // No WiFi capability

void websocket_console_start(void)
{
    printf("WebSocket console disabled; USB serial only.\n");
}

uint32_t wait_for_wifi(void)
{
    return 0;
}

bool websocket_console_is_running(void)
{
    return false;
}

#endif // CYW43_WL_GPIO_LED_PIN