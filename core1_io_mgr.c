#include "core1_io_mgr.h"

#include <stdio.h>
#include <string.h>

#include "hardware/timer.h"
#include "pico/stdlib.h"

#include "PortDrivers/files_io.h"
#include "websocket_console.h"

#ifdef DISPLAY_ST7789_SUPPORT
#include "FrontPanels/display_st7789.h"
#include "cpu_state.h"
#include "Altair8800/intel8080.h"
#endif

#ifdef INKY_SUPPORT
#include "FrontPanels/inky_display.h"
#endif

// Enable WiFi/WebSocket functionality only if board has WiFi capability
#if defined(CYW43_WL_GPIO_LED_PIN)

#include "config.h"
#include "cyw43.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "lwip/apps/mdns.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
#include "pico/unique_id.h"
#include "wifi.h"
#include "ws.h"
#include "captive_portal/captive_portal.h"
#ifdef REMOTE_FS_SUPPORT
#include "Altair8800/remote_fs.h"
#endif

#ifndef WIFI_AUTH
#define WIFI_AUTH CYW43_AUTH_WPA2_AES_PSK
#endif

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WS_OUTPUT_TIMER_INTERVAL_MS 20
#define WS_INPUT_TIMER_INTERVAL_MS 10
#define DISPLAY_UPDATE_INTERVAL_MS 40  // 25 Hz display refresh

static void websocket_console_core1_entry(void);

volatile bool console_running = false;
volatile bool console_initialized = false;
volatile bool wifi_connected = false;
volatile bool ap_mode_active = false;
volatile bool pending_ws_output = false;
volatile bool pending_ws_input = false;

char ip_address_buffer[32] = {0};
static char connected_ssid[CONFIG_SSID_MAX_LEN + 1] = {0};

// Timer for periodic WebSocket output
static struct repeating_timer ws_output_timer;

// Timer for periodic WebSocket input
static struct repeating_timer ws_input_timer;

#ifdef DISPLAY_ST7789_SUPPORT
// Timer for periodic display updates (Core 1)
static struct repeating_timer display_update_timer;
static volatile bool pending_display_update = false;

// External CPU reference for display updates
extern intel8080_t cpu;
#endif

static bool mdns_started = false;
static char mdns_hostname[32] = {0};

static void start_mdns(struct netif* netif)
{
    if (mdns_started || !netif)
    {
        return;
    }

    if (mdns_hostname[0] == '\0')
    {
        pico_unique_board_id_t board_id;
        pico_get_unique_board_id(&board_id);
        // Use last 4 bytes for a short, stable suffix
        snprintf(mdns_hostname, sizeof(mdns_hostname),
                 "altair-8800-%02x%02x%02x%02x",
                 board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 4],
                 board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 3],
                 board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 2],
                 board_id.id[PICO_UNIQUE_BOARD_ID_SIZE_BYTES - 1]);
    }

    const char* hostname = mdns_hostname;
    netif_set_hostname(netif, hostname);

    mdns_resp_init();

    s8_t err = mdns_resp_add_netif(netif, hostname);
    if (err < 0)
    {
        printf("[Core1] mDNS add netif failed (err=%d)\n", err);
        return;
    }

    mdns_resp_add_service(netif, "Altair 8800", "_http", DNSSD_PROTO_TCP, 80, NULL, NULL);
    mdns_started = true;
    printf("[Core1] mDNS started: %s.local\n", hostname);
}

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

#ifdef DISPLAY_ST7789_SUPPORT
// Timer callback for display updates - fires every 33ms (~30 Hz)
static bool display_update_timer_callback(struct repeating_timer* t)
{
    (void)t;
    pending_display_update = true;
    return true; // Keep repeating
}

// Display update function for Core 1
static inline void update_display_if_pending(void)
{
    if (!pending_display_update)
    {
        return;
    }
    pending_display_update = false;

    // Construct 10-bit status word for display:
    // Bits 0-7: CPU status byte (MEMR, INP, M1, OUT, HLTA, STACK, WO, INT)
    // Bit 9: INTE (Interrupt Enable) flag from CPU flags
    uint16_t status_word = cpu.cpuStatus;
    if (cpu.registers.flags & FLAGS_IF)
        status_word |= (1 << 9);

    // display_st7789_show_front_panel handles change detection internally
    display_st7789_show_front_panel(cpu.address_bus, cpu.data_bus, status_word);
}
#endif

// WiFi initialization result
typedef enum
{
    WIFI_INIT_OK,           // Connected to WiFi successfully
    WIFI_INIT_NO_CREDS,     // No credentials configured - start AP mode
    WIFI_INIT_CONNECT_FAIL, // Connection failed - start AP mode
    WIFI_INIT_HW_FAIL       // Hardware initialization failed
} wifi_init_result_t;

static wifi_init_result_t wifi_init(void)
{
    printf("[Core1] Initializing CYW43...\n");
    if (cyw43_arch_init())
    {
        printf("[Core1] CYW43 init failed\n");
        wifi_set_ready(false);
        return WIFI_INIT_HW_FAIL;
    }

    wifi_set_ready(true);
    cyw43_arch_enable_sta_mode();

    // Set unique hostname based on board ID BEFORE connecting
    // This ensures the hostname is sent in DHCP requests
    static char hostname[20];
    pico_unique_board_id_t board_id;
    pico_get_unique_board_id(&board_id);
    sprintf(hostname, "pico-%02x%02x%02x",
            board_id.id[5], board_id.id[6], board_id.id[7]);
    
    struct netif* netif = netif_default;
    if (netif)
    {
        netif_set_hostname(netif, hostname);
        printf("[Core1] Hostname set to: %s\n", hostname);
    }

    // Load credentials from flash storage
    char ssid[CONFIG_SSID_MAX_LEN + 1] = {0};
    char password[CONFIG_PASSWORD_MAX_LEN + 1] = {0};
    bool credentials_loaded = config_load_wifi(ssid, sizeof(ssid), password, sizeof(password));

    // Check if credentials were loaded successfully
    if (!credentials_loaded || ssid[0] == '\0')
    {
        printf("[Core1] No WiFi credentials configured, switching to AP mode\n");
        return WIFI_INIT_NO_CREDS;
    }

    printf("[Core1] Using stored credentials from flash\n");

    printf("[Core1] Connecting to Wi-Fi SSID '%s'...\n", ssid);

    // Store the SSID we're connecting to
    strncpy(connected_ssid, ssid, sizeof(connected_ssid) - 1);
    connected_ssid[sizeof(connected_ssid) - 1] = '\0';

    int err = cyw43_arch_wifi_connect_timeout_ms(ssid, password, WIFI_AUTH, WIFI_CONNECT_TIMEOUT_MS);

    if (err != 0)
    {
        printf("[Core1] Wi-Fi connect failed (err=%d), switching to AP mode\n", err);
        wifi_set_connected(false);
        return WIFI_INIT_CONNECT_FAIL;
    }

    wifi_set_connected(true);

    // Disable WiFi power management completely for lowest latency
    // With threadsafe_background, the driver is serviced by interrupt,
    // but power save can still add latency when waking from sleep
    cyw43_wifi_pm(&cyw43_state, CYW43_NO_POWERSAVE_MODE);

    // Get and store IP address
    netif = netif_default;
    if (netif && netif_is_up(netif))
    {
        const ip4_addr_t* addr = netif_ip4_addr(netif);
        if (addr)
        {
            ip4addr_ntoa_r(addr, ip_address_buffer, sizeof(ip_address_buffer));
            wifi_set_ip_address(ip_address_buffer); // Cache for display
        }
        // mDNS disabled - causes significant latency spikes (500ms-2.5s)
        // The lwIP mDNS responder blocks during multicast announcements
        // start_mdns(netif);
    }

    printf("[Core1] Wi-Fi connected. IP: %s\n", ip_address_buffer);
    return WIFI_INIT_OK;
}

void websocket_console_start(void)
{
    if (console_running)
    {
        return;
    }

    websocket_queue_init();
    files_io_init(); // Initialize file transfer client queues

#ifdef REMOTE_FS_SUPPORT
    // Initialize remote FS client queues
    rfs_client_init();
#endif

    // Launch core 1 which will handle all Wi-Fi and WebSocket operations
    // Timers are started on core 1 so callbacks execute there
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

bool is_ap_mode_active(void)
{
    return ap_mode_active;
}

bool websocket_console_is_running(void)
{
    return console_running && wifi_connected && ws_is_running();
}

const char* get_mdns_hostname(void)
{
    return mdns_started && mdns_hostname[0] != '\0' ? mdns_hostname : NULL;
}

static void websocket_console_core1_entry(void)
{
    // Start timers on core 1 so callbacks execute here
    add_repeating_timer_ms(-WS_OUTPUT_TIMER_INTERVAL_MS, ws_output_timer_callback, NULL, &ws_output_timer);
    printf("[Core1] Started WebSocket output timer (%dms interval)\n", WS_OUTPUT_TIMER_INTERVAL_MS);

    add_repeating_timer_ms(-WS_INPUT_TIMER_INTERVAL_MS, ws_input_timer_callback, NULL, &ws_input_timer);
    printf("[Core1] Started WebSocket input timer (%dms interval)\n", WS_INPUT_TIMER_INTERVAL_MS);

#ifdef INKY_SUPPORT
    inky_display_init();
    printf("[Core1] Inky display initialized\n");
#endif

#ifdef DISPLAY_ST7789_SUPPORT
    add_repeating_timer_ms(-DISPLAY_UPDATE_INTERVAL_MS, display_update_timer_callback, NULL, &display_update_timer);
    printf("[Core1] Started display update timer (%dms interval, ~30 Hz)\n", DISPLAY_UPDATE_INTERVAL_MS);
    display_st7789_init();
    display_st7789_init_front_panel();
    printf("[Core1] Virtual Front Panel initialized\n");
#endif

    // Initialize Wi-Fi on core 1
    wifi_init_result_t result = wifi_init();
    wifi_connected = (result == WIFI_INIT_OK);

    // Signal core 0: send 0 for failure, or raw IP address (32-bit) for success
    // For AP mode, we send a special value (0xFFFFFFFF) to indicate AP mode is active
    uint32_t ip_raw = 0;

    if (result == WIFI_INIT_OK)
    {
        // STA mode - connected to WiFi
        struct netif* netif = netif_default;
        if (netif && netif_is_up(netif))
        {
            const ip4_addr_t* addr = netif_ip4_addr(netif);
            if (addr)
            {
                ip_raw = ip4_addr_get_u32(addr);
            }
        }
        multicore_fifo_push_blocking(ip_raw);

        // Initialize and start WebSocket server
        if (!websocket_console_init_server())
        {
            printf("[Core1] Failed to start WebSocket server\n");
            return;
        }

        // Mark console as initialized only after successful network stack initialization
        console_initialized = true;
        printf("[Core1] WebSocket server running, entering poll loop\n");

#ifdef DISPLAY_ST7789_SUPPORT
        // Update display with WiFi info now that we're connected
        display_st7789_update(connected_ssid, ip_address_buffer);
        printf("[Core1] Display updated with WiFi info\n");
#endif

        // Main poll loop - CYW43/lwIP is handled by background interrupt
        // with pico_cyw43_arch_lwip_threadsafe_background
        while (true)
        {
            ws_poll(&pending_ws_input, &pending_ws_output);
#ifdef REMOTE_FS_SUPPORT
            // Poll remote FS client
            rfs_client_poll();
#endif
            ft_client_poll(); // Poll file transfer client
#ifdef DISPLAY_ST7789_SUPPORT
            update_display_if_pending(); // Update display if timer triggered
#endif
            tight_loop_contents();
        }
    }
    else if (result == WIFI_INIT_NO_CREDS || result == WIFI_INIT_CONNECT_FAIL)
    {
        // Start captive portal in AP mode
        printf("[Core1] Starting captive portal for WiFi configuration...\n");

        if (captive_portal_start())
        {
            ap_mode_active = true;
            // Signal core 0 with special value (0xFFFFFFFF) to indicate AP mode
            // Core 0 will then spin in RAM, which is safe during flash operations
            multicore_fifo_push_blocking(0xFFFFFFFF);

            // AP mode poll loop
            printf("[Core1] Captive portal running, entering poll loop\n");
            while (true)
            {
                captive_portal_poll();
                tight_loop_contents();
            }
        }
        else
        {
            printf("[Core1] Failed to start captive portal\n");
            multicore_fifo_push_blocking(0);
            return;
        }
    }
    else
    {
        // Hardware failure
        multicore_fifo_push_blocking(0);
        printf("[Core1] Wi-Fi hardware unavailable, network task exiting\n");
        return;
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

bool is_ap_mode_active(void)
{
    return false;
}

#endif // CYW43_WL_GPIO_LED_PIN