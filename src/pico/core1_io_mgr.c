#include "core1_io_mgr.h"

#include <stdio.h>
#include <string.h>

#include "hardware/timer.h"
#include "pico/stdlib.h"

#include "PortDrivers/files_io.h"
#include "drivers/bluetooth/bt_keyboard.h"
#include "websocket_console.h"

#ifdef DISPLAY_ST7789_SUPPORT
#include "FrontPanels/display_st7789.h"
#include "cpu_state.h"
#include "Altair8800/intel8080.h"
#endif

#ifdef WAVESHARE_3_5_DISPLAY
#include "drivers/waveshare/ws_display.h"
#ifndef DISPLAY_ST7789_SUPPORT
#include "cpu_state.h"
#include "Altair8800/intel8080.h"
#endif
#endif

#ifdef INKY_SUPPORT
#include "FrontPanels/inky_display.h"
#endif

#ifdef VT100_DISPLAY
#include "FrontPanels/vt100_display.h"
#include "drivers/waveshare/ws_ili9488.h"
#endif

// Enable WiFi/WebSocket functionality only if board has WiFi capability
#if defined(CYW43_WL_GPIO_LED_PIN)

#include "config.h"
#include "cyw43.h"
#include "lwip/ip4_addr.h"
#include "lwip/netif.h"
#include "pico/cyw43_arch.h"
#include "pico/multicore.h"
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

#if defined(WAVESHARE_3_5_DISPLAY) && defined(BLUETOOTH_KEYBOARD_SUPPORT)
#define DISPLAY_UPDATE_INTERVAL_MS 100  // 10 Hz to leave core-1 headroom for BLE + display DMA
#else
#define DISPLAY_UPDATE_INTERVAL_MS 20   // 50 Hz display refresh
#endif

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

#if defined(WAVESHARE_3_5_DISPLAY) && !defined(DISPLAY_ST7789_SUPPORT)
// Timer for periodic ILI9488 display updates (Core 1)
static struct repeating_timer display_update_timer;
static volatile bool pending_display_update = false;

// External CPU reference for display updates
extern intel8080_t cpu;
#endif

#ifdef VT100_DISPLAY
#define VT100_UPDATE_INTERVAL_MS 50  // 20 Hz
static struct repeating_timer vt100_update_timer;
static volatile bool pending_vt100_update = false;
extern intel8080_t cpu;

static bool vt100_update_timer_callback(struct repeating_timer* t)
{
    (void)t;
    pending_vt100_update = true;
    return true;
}
#endif

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
// Timer callback for display updates - fires every 20ms (~50 Hz)
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

#if defined(WAVESHARE_3_5_DISPLAY) && !defined(DISPLAY_ST7789_SUPPORT)
// Timer callback for ILI9488 display updates - fires every 20ms (~50 Hz)
static bool display_update_timer_callback(struct repeating_timer* t)
{
    (void)t;
    pending_display_update = true;
    return true;
}

// ILI9488 display update function for Core 1
static inline void update_display_if_pending(void)
{
    if (!pending_display_update)
    {
        return;
    }
    pending_display_update = false;

    uint16_t status_word = cpu.cpuStatus;
    if (cpu.registers.flags & FLAGS_IF)
        status_word |= (1 << 9);

    ws_display_show_front_panel(cpu.address_bus, cpu.data_bus, status_word);
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

#ifdef BLUETOOTH_KEYBOARD_SUPPORT
    bt_keyboard_init();
#endif

    cyw43_arch_enable_sta_mode();

    // Set unique hostname based on board ID BEFORE connecting
    // This ensures the hostname is sent in DHCP requests
    const char* hostname = wifi_get_hostname();
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

#ifdef VT100_DISPLAY
    ws_ili9488_init();
    vt100_init();
    add_repeating_timer_ms(-VT100_UPDATE_INTERVAL_MS, vt100_update_timer_callback, NULL, &vt100_update_timer);
    printf("[Core1] VT100 terminal display initialized (20 Hz refresh)\n");
#endif

#if !defined(VT100_DISPLAY)
#ifdef DISPLAY_ST7789_SUPPORT
    add_repeating_timer_ms(-DISPLAY_UPDATE_INTERVAL_MS, display_update_timer_callback, NULL, &display_update_timer);
    printf("[Core1] Started display update timer (%dms interval, ~50 Hz)\n", DISPLAY_UPDATE_INTERVAL_MS);
    display_st7789_init();
    display_st7789_init_front_panel();
    printf("[Core1] Virtual Front Panel initialized\n");
#endif

#if defined(WAVESHARE_3_5_DISPLAY) && !defined(DISPLAY_ST7789_SUPPORT)
    add_repeating_timer_ms(-DISPLAY_UPDATE_INTERVAL_MS, display_update_timer_callback, NULL, &display_update_timer);
    printf("[Core1] Started Waveshare display update timer (%dms interval, ~50 Hz)\n", DISPLAY_UPDATE_INTERVAL_MS);
    ws_display_init();
    ws_display_init_front_panel();
    printf("[Core1] Waveshare Virtual Front Panel initialized\n");
#endif
#endif /* !VT100_DISPLAY */

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

#ifndef VT100_DISPLAY
#ifdef DISPLAY_ST7789_SUPPORT
        // Update display with WiFi info now that we're connected
        display_st7789_update(connected_ssid, ip_address_buffer);
        printf("[Core1] Display updated with WiFi info\n");
#endif

#if defined(WAVESHARE_3_5_DISPLAY) && !defined(DISPLAY_ST7789_SUPPORT)
        ws_display_update_wifi(connected_ssid, ip_address_buffer);
        printf("[Core1] Waveshare display updated with WiFi info\n");
#endif
#else
        vt100_set_ip(ip_address_buffer);
        printf("[Core1] VT100 status bar updated with IP\n");
#endif /* !VT100_DISPLAY */

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
#ifdef BLUETOOTH_KEYBOARD_SUPPORT
            bt_keyboard_poll();
#endif
#ifdef VT100_DISPLAY
            /* Drain the VT100 output queue every iteration.
             * Update the status bar and service the terminal on the 20 Hz timer. */
            {
                uint8_t vt_ch;
                while (vt100_try_dequeue_output(&vt_ch)) {
                    vt100_putchar(vt_ch);
                }

                if (pending_vt100_update) {
                    pending_vt100_update = false;
                    uint16_t sw = cpu.cpuStatus;
                    if (cpu.registers.flags & FLAGS_IF)
                        sw |= (1 << 9);
                    vt100_update_status(cpu.address_bus, cpu.data_bus, sw);
                    vt100_service();
                }
            }
#else /* front panel mode */
#ifdef DISPLAY_ST7789_SUPPORT
            update_display_if_pending(); // Update display if timer triggered
#endif
#if defined(WAVESHARE_3_5_DISPLAY) && !defined(DISPLAY_ST7789_SUPPORT)
            update_display_if_pending(); // Update Waveshare display if timer triggered
            ws_display_poll();           // Progress async DMA queue/completion
#endif
#endif /* VT100_DISPLAY */
            tight_loop_contents();
        }
    }
    else if (result == WIFI_INIT_NO_CREDS || result == WIFI_INIT_CONNECT_FAIL)
    {
#if defined(VT100_DISPLAY) && defined(BLUETOOTH_KEYBOARD_SUPPORT)
        // VT100 + BT keyboard mode: WiFi failed but that's OK - continue
        // without it. Power down WiFi radio to save power.
        printf("[Core1] WiFi unavailable, continuing in BT-only mode\n");
        cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);
        multicore_fifo_push_blocking(0);
        vt100_set_ip("BT only");

        // BT-only + VT100 display poll loop
        while (true)
        {
            bt_keyboard_poll();
            {
                uint8_t vt_ch;
                while (vt100_try_dequeue_output(&vt_ch)) {
                    vt100_putchar(vt_ch);
                }

                if (pending_vt100_update) {
                    pending_vt100_update = false;
                    uint16_t sw = cpu.cpuStatus;
                    if (cpu.registers.flags & FLAGS_IF)
                        sw |= (1 << 9);
                    vt100_update_status(cpu.address_bus, cpu.data_bus, sw);
                    vt100_service();
                }
            }
            tight_loop_contents();
        }
#else
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
#endif /* VT100_DISPLAY && BLUETOOTH_KEYBOARD_SUPPORT */
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
