#include "Altair8800/intel8080.h"
#include "Altair8800/memory.h"
#if defined(SD_CARD_SUPPORT)
#include "Altair8800/pico_88dcdd_sd_card.h"
#include "diskio.h"
#include "drivers/sdcard/sdcard.h"
#include "ff.h"
#elif defined(REMOTE_FS_SUPPORT)
#include "Altair8800/pico_88dcdd_remote_fs.h"
#include "Altair8800/remote_fs.h"
#else
#include "Altair8800/pico_88dcdd_flash.h"
#endif
#include "FrontPanels/display_st7789.h"
#include "FrontPanels/inky_display.h"
#include "build_version.h"
#include "comms_mgr.h"
#include "config.h"
#include "cpu_state.h"
#include "hardware/timer.h"
#include "io_ports.h"
#include "pico/error.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASCII_MASK_7BIT 0x7F
#define CTRL_KEY(ch) ((ch) & 0x1F)

#if !defined(SD_CARD_SUPPORT) && !defined(REMOTE_FS_SUPPORT)
// Include the CPM disk image (only for embedded XIP disk controller)
#include "Disks/bdsc_v1_60_disk.h"
#include "Disks/cpm63k_disk.h"
#endif

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
    if (config_exists())
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
    if (!config_prompt_and_save(config_timeout))
    {
        // User didn't configure, use existing credentials if available
        if (config_exists())
        {
            printf("Using stored WiFi credentials\n");
        }
        else
        {
            printf("No WiFi credentials configured - WiFi will be unavailable\n");
        }
    }

#if defined(REMOTE_FS_SUPPORT) && !defined(SD_CARD_SUPPORT)
    // Initialize remote FS client queues BEFORE Core 1 starts polling
    rfs_client_init();
#endif

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

#ifdef DISPLAY_ST7789_SUPPORT
// Global flag set by timer callback every 20ms
static volatile bool display_update_pending = false;

// Timer callback - fires every 33ms (~30 Hz)
static bool display_timer_callback(struct repeating_timer* t)
{
    display_update_pending = true;
    return true; // Keep repeating
}

// Function to update the display (called from main loop)
static inline void update_display_if_changed(void)
{
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

int main(void)
{
    // Initialize stdio first
    stdio_init_all();

    // Disable stdout buffering - critical for seeing debug output before crashes
    // setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== BOOT START ===\n");
#if defined(REMOTE_FS_SUPPORT)
    printf("=== REMOTE_FS_SUPPORT is defined ===\n");
#endif

    // Initialize displays early (if enabled)
    inky_display_init();
    display_st7789_init();

#if defined(CYW43_WL_GPIO_LED_PIN)
    // Board has WiFi - check if credentials exist
    config_init();

    if (!config_exists())
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

#if defined(CYW43_WL_GPIO_LED_PIN)
    printf("HTTP file transfer: Enabled (gf command supported)\n");
#else
    printf("HTTP file transfer: Disabled (no WiFi)\n");
#endif
    printf("\n");

    // Initialize disk controller
    printf("Initializing disk controller...\n");
#if defined(SD_CARD_SUPPORT)
    sd_disk_init();
#elif defined(REMOTE_FS_SUPPORT)
    rfs_disk_init();
#else
    pico_disk_init();
#endif

#ifdef SD_CARD_SUPPORT
    // Initialize and mount SD card
    printf("Initializing SD card...\n");

#ifdef WAVESHARE_3_5_DISPLAY
    // Waveshare 3.5" display shares SPI1 with LCD (CS=9), Touch (CS=16), and SD (CS=22)
    // We must ensure LCD and Touch CS pins are HIGH to prevent SPI bus interference
    gpio_init(9);
    gpio_set_dir(9, GPIO_OUT);
    gpio_put(9, 1); // Deselect LCD
    gpio_init(16);
    gpio_set_dir(16, GPIO_OUT);
    gpio_put(16, 1); // Deselect Touch
#endif

    static FATFS fs;
    FRESULT fr = f_mount(&fs, "", 1); // Immediate mount (calls disk_initialize internally)

    if (fr != FR_OK)
    {
        printf("Failed to mount SD card, error: %d\n", fr);
        printf("Possible causes:\n");
        printf("  - No SD card inserted\n");
        printf("  - SD card not formatted as FAT32\n");
        printf("  - Pin conflict with other peripherals\n");
        return -1;
    }

    printf("SD card mounted successfully.\n");

    // Load disk images from SD card
    printf("Opening DISK_A: %s\n", DISK_A_PATH);
    if (sd_disk_load(0, DISK_A_PATH))
    {
        printf("DISK_A opened successfully\n");
    }
    else
    {
        printf("DISK_A initialization failed!\n");
        return -1;
    }

    printf("Opening DISK_B: %s\n", DISK_B_PATH);
    if (sd_disk_load(1, DISK_B_PATH))
    {
        printf("DISK_B opened successfully\n");
    }
    else
    {
        printf("DISK_B initialization failed!\n");
        return -1;
    }

    printf("Opening DISK_C: %s\n", DISK_C_PATH);
    if (sd_disk_load(2, DISK_C_PATH))
    {
        printf("DISK_C opened successfully\n");
    }
    else
    {
        printf("DISK_C initialization failed!\n");
        return -1;
    }

    printf("Opening DISK_D: %s\n", DISK_D_PATH);
    if (sd_disk_load(3, DISK_D_PATH))
    {
        printf("DISK_D opened successfully\n");
    }
    else
    {
        printf("DISK_D initialization failed!\n");
        return -1;
    }
#elif defined(REMOTE_FS_SUPPORT)
    // Connect to remote FS server
    printf(">>> REMOTE_FS: About to connect...\n");
    fflush(stdout);
    if (!rfs_disk_connect())
    {
        printf("Failed to connect to remote FS server!\n");
        printf("Ensure remote_fs_server.py is running on the network.\n");
        fflush(stdout);
        return -1;
    }
    printf("Remote FS connected - all disks available from server.\n");
    fflush(stdout);
#else
    // Load CPM disk image into drive 0 (DISK_A)
    printf("Opening DISK_A: cpm63k.dsk (embedded)\n");
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
    printf("Opening DISK_B: bdsc_v1_60.dsk (embedded)\n");
    if (pico_disk_load(1, bdsc_v1_60_dsk, bdsc_v1_60_dsk_len))
    {
        printf("DISK_B opened successfully (%u bytes)\n", bdsc_v1_60_dsk_len);
    }
    else
    {
        printf("DISK_B initialization failed!\n");
        return -1;
    }
#endif

    // Load disk boot loader ROM at 0xFF00 (ROM_LOADER_ADDRESS)
    printf("Loading disk boot loader ROM at 0xFF00...\n");
    loadDiskLoader(0xFF00);

    // Set up disk controller structure for CPU
#if defined(SD_CARD_SUPPORT)
    static disk_controller_t disk_controller = {.disk_select = (port_out)sd_disk_select,
                                                .disk_status = (port_in)sd_disk_status,
                                                .disk_function = (port_out)sd_disk_function,
                                                .sector = (port_in)sd_disk_sector,
                                                .write = (port_out)sd_disk_write,
                                                .read = (port_in)sd_disk_read};
#elif defined(REMOTE_FS_SUPPORT)
    static disk_controller_t disk_controller = {.disk_select = (port_out)rfs_disk_select,
                                                .disk_status = (port_in)rfs_disk_status,
                                                .disk_function = (port_out)rfs_disk_function,
                                                .sector = (port_in)rfs_disk_sector,
                                                .write = (port_out)rfs_disk_write,
                                                .read = (port_in)rfs_disk_read};
#else
    static disk_controller_t disk_controller = {.disk_select = (port_out)pico_disk_select,
                                                .disk_status = (port_in)pico_disk_status,
                                                .disk_function = (port_out)pico_disk_function,
                                                .sector = (port_in)pico_disk_sector,
                                                .write = (port_out)pico_disk_write,
                                                .read = (port_in)pico_disk_read};
#endif

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
    display_st7789_update(wifi_ssid, g_wifi_ok ? g_ip_buffer : NULL);
#else
    // No WiFi on this board
    inky_display_update(NULL, NULL);
    display_st7789_update(NULL, NULL);
#endif

#ifdef DISPLAY_ST7789_SUPPORT
    printf("\n*** Virtual Front Panel (Core 0 Enabled - Polling) ***\n");
    display_st7789_init_front_panel();

    // Start hardware timer for display updates (33ms = ~30 Hz)
    static struct repeating_timer display_timer;
    add_repeating_timer_ms(-33, display_timer_callback, NULL, &display_timer);
    printf("Display update timer started (~30 Hz)\n");
#endif
    // ============================================

    // Main emulation loop - core 0 dedicated to CPU emulation
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

#ifdef DISPLAY_ST7789_SUPPORT
        // Check if display update is pending (set by timer callback every 20ms)
        if (display_update_pending)
        {
            display_update_pending = false;
            update_display_if_changed();
        }
#endif
    }
}
