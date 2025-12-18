#include "Altair8800/intel8080.h"
#include "Altair8800/memory.h"
#include "Altair8800/pico_disk.h"
#include "build_version.h"
#include "comms_mgr.h"
#include "cpu_state.h"
#include "FrontPanels/inky_display.h"
#include "FrontPanels/display_2_8.h"
#include "io_ports.h"
#include "pico/error.h"
#include "pico/stdlib.h"
#include "wifi.h"
#include "wifi_config.h"
#include <stdio.h>
#include <string.h>

#define ASCII_MASK_7BIT 0x7F
#define CTRL_KEY(ch) ((ch) & 0x1F)

// Include the CPM disk image
#include "Disks/blank_disk.h"
#include "Disks/cpm63k_disk.h"

// WiFi connection status (global for Inky display)
static bool g_wifi_ok = false;
static char g_ip_buffer[32] = {0};

// CPU instance defined in cpu_state.c
extern intel8080_t cpu;

// Forward declarations of static functions
static uint8_t terminal_read(void);
static void terminal_write(uint8_t c);
static inline uint8_t sense(void);

// Static disk controller reference for reset
static disk_controller_t* g_disk_controller = NULL;

void client_connected_cb(void)
{
    cpu_state_set_mode(CPU_RUNNING);
}

// Reset function for CPU monitor
void altair_reset(void)
{
    if (g_disk_controller)
    {
        memset(memory, 0x00, 64 * 1024); // Clear Altair memory
        loadDiskLoader(0xFF00);          // Load disk boot loader at 0xFF00
        i8080_reset(&cpu, terminal_read, terminal_write, sense, g_disk_controller, io_port_in, io_port_out);
        i8080_examine(&cpu, 0xFF00); // Reset to boot loader address
        bus_switches = cpu.address_bus;
    }
}

// Process character through ANSI escape sequence state machine
static uint8_t process_ansi_sequence(uint8_t ch)
{
    // Translate ANSI cursor sequences to the control keys CP/M expects (WordStar style).
    enum
    {
        KEY_STATE_NORMAL = 0,
        KEY_STATE_ESC,
        KEY_STATE_ESC_BRACKET,
        KEY_STATE_ESC_BRACKET_NUM
    };

    static uint8_t key_state = KEY_STATE_NORMAL;
    static uint8_t pending_key = 0;

    switch (key_state)
    {
        case KEY_STATE_NORMAL:
            if (ch == 0x1B)
            {
                key_state = KEY_STATE_ESC;
                return 0x00; // Start of escape sequence
            }
            if (ch == 0x7F || ch == 0x08)
            {
                return (uint8_t)CTRL_KEY('H'); // Map delete/backspace to Ctrl-H (0x08)
            }
            return ch;

        case KEY_STATE_ESC:
            if (ch == '[')
            {
                key_state = KEY_STATE_ESC_BRACKET;
                return 0x00; // Control sequence introducer
            }
            key_state = KEY_STATE_NORMAL;
            return ch; // Pass through unknown sequences

        case KEY_STATE_ESC_BRACKET:
            switch (ch)
            {
                case 'A':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('E'); // Up -> Ctrl-E
                case 'B':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('X'); // Down -> Ctrl-X
                case 'C':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('D'); // Right -> Ctrl-D
                case 'D':
                    key_state = KEY_STATE_NORMAL;
                    return (uint8_t)CTRL_KEY('S'); // Left -> Ctrl-S
                case '2':
                    // Insert key sends ESC[2~ - need to consume the tilde
                    pending_key = (uint8_t)CTRL_KEY('O'); // Insert -> Ctrl-O
                    key_state = KEY_STATE_ESC_BRACKET_NUM;
                    return 0x00;
                case '3':
                    // Delete key sends ESC[3~ - need to consume the tilde
                    pending_key = (uint8_t)CTRL_KEY('G'); // Delete -> Ctrl-G
                    key_state = KEY_STATE_ESC_BRACKET_NUM;
                    return 0x00;
                default:
                    key_state = KEY_STATE_NORMAL;
                    return 0x00; // Ignore other sequences
            }

        case KEY_STATE_ESC_BRACKET_NUM:
            key_state = KEY_STATE_NORMAL;
            if (ch == '~')
            {
                // Return the pending key now that we've consumed the tilde
                uint8_t result = pending_key;
                pending_key = 0;
                return result;
            }
            pending_key = 0;
            return 0x00; // Unexpected character, ignore
    }

    key_state = KEY_STATE_NORMAL;
    return 0x00;
}

// Terminal read function - non-blocking
static uint8_t terminal_read(void)
{

#if defined(CYW43_WL_GPIO_LED_PIN)
    uint8_t ws_ch = 0;
    if (websocket_console_try_dequeue_input(&ws_ch))
    {
        return (uint8_t)(ws_ch & ASCII_MASK_7BIT);
    }
#else
    int c = getchar_timeout_us(0); // Non-blocking read
    if (c == PICO_ERROR_TIMEOUT)
    {
        return 0x00; // Return null if no character available
    }

    uint8_t ch = (uint8_t)(c & ASCII_MASK_7BIT);
    return process_ansi_sequence(ch);
#endif
}

// Terminal write function
static void terminal_write(uint8_t c)
{
    c &= ASCII_MASK_7BIT; // Take first 7 bits only
#if defined(CYW43_WL_GPIO_LED_PIN)
    websocket_console_enqueue_output(c);
#else
    putchar(c);
#endif
}

// Sense switches
static inline uint8_t sense(void)
{
    return (uint8_t)(bus_switches >> 8);
}

// Initialize and configure WiFi
static void setup_wifi(void)
{

    // WiFi configuration system already initialized in main()

    // Determine timeout based on whether credentials exist
    uint32_t config_timeout;
    if (wifi_config_exists())
    {
        printf("\nWiFi credentials found in flash storage.\n");
        config_timeout = 5000; // 5 seconds if credentials exist
    }
    else
    {
        printf("\nNo WiFi credentials found in flash storage.\n");
        config_timeout = 15000; // 15 seconds if no credentials
    }

    // Give user the option to configure/update WiFi
    if (!wifi_config_prompt_and_save(config_timeout))
    {
        // User didn't configure, use existing credentials if available
        if (wifi_config_exists())
        {
            printf("Using stored WiFi credentials\n");
        }
        else
        {
            printf("No WiFi credentials configured - WiFi will be unavailable\n");
        }
    }

    // Launch network task on core 1 (handles Wi-Fi init, WebSocket server, polling)
    websocket_console_start();

    // Wait for core 1 to complete Wi-Fi initialization
    // Returns 0 on failure, or raw 32-bit IP address on success
    printf("Waiting for Wi-Fi initialization on core 1...\n");
    uint32_t ip_raw = wait_for_wifi();
    g_wifi_ok = (ip_raw != 0);

    if (g_wifi_ok)
    {
        // Convert raw IP to dotted-decimal string
        snprintf(g_ip_buffer, sizeof(g_ip_buffer), "%lu.%lu.%lu.%lu", (unsigned long)(ip_raw & 0xFF),
                 (unsigned long)((ip_raw >> 8) & 0xFF), (unsigned long)((ip_raw >> 16) & 0xFF),
                 (unsigned long)((ip_raw >> 24) & 0xFF));
        printf("Wi-Fi connected. IP: %s\n", g_ip_buffer);
    }
    else
    {
        strncpy(g_ip_buffer, "No network", sizeof(g_ip_buffer) - 1);
        printf("Wi-Fi unavailable; USB terminal only.\n");
    }
}

int main(void)
{
    // Initialize stdio first
    stdio_init_all();

    // Initialize displays early (if enabled)
    inky_display_init();
    display_2_8_init();

#if defined(CYW43_WL_GPIO_LED_PIN)
    // Board has WiFi - check if credentials exist
    wifi_config_init();

    if (!wifi_config_exists())
    {
        // No credentials - wait for USB serial connection so user sees WiFi config prompt
        while (!stdio_usb_connected())
        {
            sleep_ms(100);
        }
        // Give a brief moment after connection for terminal to be ready
        sleep_ms(500);
    }
    else
    {
        // Credentials exist - wait up to 10 seconds for USB connection, then proceed
        absolute_time_t start_time = get_absolute_time();
        while (!stdio_usb_connected() && absolute_time_diff_us(start_time, get_absolute_time()) < 10000000)
        {
            sleep_ms(100);
        }
        // Give a brief moment after connection for terminal to be ready
        if (stdio_usb_connected())
        {
            sleep_ms(500);
        }
    }

    setup_wifi();
#else
    // Board has no WiFi - wait for USB serial connection before proceeding
    // This ensures we don't miss any output on non-WiFi boards
    while (!stdio_usb_connected())
    {
        sleep_ms(100);
    }
    // Give a brief moment after connection for terminal to be ready
    sleep_ms(500);

    cpu_state_set_mode(CPU_RUNNING);
#endif

    // Send test output
    printf("\n\n*** USB Serial Active ***\n");
    printf("========================================\n");
    printf("  Altair 8800 Emulator\n");
    printf("  Board: %s\n", PICO_BOARD);
    printf("  Build: %d (%s %s)\n", BUILD_VERSION, BUILD_DATE, BUILD_TIME);
    printf("========================================\n");
    printf("\n");

    // Initialize disk controller
    printf("Initializing disk controller...\n");
    pico_disk_init();

    // Load CPM disk image into drive 0 (DISK_A)
    printf("Opening DISK_A: cpm63k.dsk\n");
    if (pico_disk_load(0, cpm63k_dsk, cpm63k_dsk_len))
    {
        printf("DISK_A opened successfully (%u bytes)\n", cpm63k_dsk_len);
    }
    else
    {
        printf("DISK_A initialization failed!\n");
        return -1;
    }

    // Load blank disk image into drive 1 (DISK_B)
    printf("Opening DISK_B: blank.dsk\n");
    if (pico_disk_load(1, blank_disk, blank_disk_len))
    {
        printf("DISK_B opened successfully (%u bytes)\n", blank_disk_len);
    }
    else
    {
        printf("DISK_B initialization failed!\n");
        return -1;
    }

    // Load disk boot loader ROM at 0xFF00 (ROM_LOADER_ADDRESS)
    printf("Loading disk boot loader ROM at 0xFF00...\n");
    loadDiskLoader(0xFF00);

    // Set up disk controller structure for CPU
    static disk_controller_t disk_controller = {.disk_select = (port_out)pico_disk_select,
                                                .disk_status = (port_in)pico_disk_status,
                                                .disk_function = (port_out)pico_disk_function,
                                                .sector = (port_in)pico_disk_sector,
                                                .write = (port_out)pico_disk_write,
                                                .read = (port_in)pico_disk_read};

    // Store reference for reset function
    g_disk_controller = &disk_controller;

    // Reset and initialize the CPU
    printf("Initializing Intel 8080 CPU...\n");
    i8080_reset(&cpu, terminal_read, terminal_write, sense, &disk_controller, io_port_in, io_port_out);

    // Set CPU to start at ROM_LOADER_ADDRESS (0xFF00) to boot from disk
    printf("Setting CPU to ROM_LOADER_ADDRESS (0xFF00) to boot from disk\n");
    i8080_examine(&cpu, 0xFF00);

    // Report basic memory usage at startup (static allocation only)
    extern char __StackLimit, __bss_end__;
    extern char __flash_binary_end;

    uint32_t heap_free = (uint32_t)(&__StackLimit - &__bss_end__);

    // SDK provides SRAM_BASE and SRAM_END in hardware/regs/addressmap.h
    // RP2040: 0x20042000 - 0x20000000 = 264 KB
    // RP2350: 0x20082000 - 0x20000000 = 520 KB
    uint32_t total_ram = SRAM_END - SRAM_BASE;
    uint32_t used_ram = total_ram - heap_free;
    uint32_t flash_used = (uint32_t)&__flash_binary_end;

    // SDK provides PICO_FLASH_SIZE_BYTES based on board configuration
#ifndef PICO_FLASH_SIZE_BYTES
    uint32_t total_flash = 2 * 1024 * 1024; // Default to 2MB
#else
    uint32_t total_flash = PICO_FLASH_SIZE_BYTES;
#endif

    printf("\n");
    printf("Memory Report:\n");
    printf("  Flash used:     %lu / %lu bytes (%.1f / %.1f KB)\n", flash_used, total_flash, flash_used / 1024.0f,
           total_flash / 1024.0f);
    printf("  RAM used:       %lu bytes (%.1f KB)\n", used_ram, used_ram / 1024.0f);
    printf("  RAM free (heap):%lu bytes (%.1f KB)\n", heap_free, heap_free / 1024.0f);
    printf("  Total SRAM:     %lu bytes (%.1f KB)\n", total_ram, total_ram / 1024.0f);
    printf("  Altair memory:  65536 bytes (64 KB)\n");
    printf("\n");

    printf("Starting Altair 8800 emulation...\n");
    printf("\n");

#if defined(CYW43_WL_GPIO_LED_PIN)
    // Update displays with system information (if enabled)
    const char* wifi_ssid = g_wifi_ok ? get_connected_ssid() : NULL;
    inky_display_update(wifi_ssid, g_wifi_ok ? g_ip_buffer : NULL);
    display_2_8_update(wifi_ssid, g_wifi_ok ? g_ip_buffer : NULL);
#else
    // No WiFi on this board
    inky_display_update(NULL, NULL);
    display_2_8_update(NULL, NULL);
#endif

    // Main emulation loop - core 0 dedicated to CPU emulation
    for (;;)
    {
        CPU_OPERATING_MODE mode = cpu_state_get_mode();
        switch (mode)
        {
            case CPU_RUNNING:
                for (int i = 0; i < 1000; ++i)
                {
                    i8080_cycle(&cpu);
                }
                break;
            case CPU_LOW_POWER:
                i8080_cycle(&cpu);
                sleep_us(1);
                break;
            case CPU_STOPPED:
            {
                uint8_t ch = 0;
                if (websocket_console_try_dequeue_monitor_input(&ch))
                {
                    // Process monitor input character
                    process_control_panel_commands_char(ch);
                }
            }
            break;
            default:
                tight_loop_contents(); // hint to compiler; no-op but lowers power
                break;
        }
    }
}
