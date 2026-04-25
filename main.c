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
#ifdef WAVESHARE_3_5_DISPLAY
#include "drivers/waveshare/ws_fatfs.h"
#include "drivers/waveshare/ws_display.h"
#include "drivers/waveshare/ws_spi1_bus.h"
#endif
#include "drivers/bluetooth/bt_keyboard.h"
#include "FrontPanels/inky_display.h"
#include "ansi_input.h"
#include "build_version.h"
#include "core1_io_mgr.h"
#include "config.h"
#include "cpu_state.h"
#include "hardware/timer.h"
#include "hardware/watchdog.h"
#include "io_ports.h"
#include "pico/error.h"
#include "pico/stdlib.h"
#include "pico/platform.h"
#include "pico/flash.h"
#include "wifi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ASCII_MASK_7BIT 0x7F

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
#if defined(BLUETOOTH_KEYBOARD_SUPPORT)
static int serial_wait_for_char_ms(uint32_t timeout_ms);
static void maybe_run_bluetooth_keyboard_shell(void);
#endif

// RAM-resident spin function for AP mode - must not execute from flash
// during flash erase/program operations. Disables interrupts to prevent
// any interrupt handlers from executing from flash.
static void __not_in_flash_func(spin_in_ram)(void)
{
    // Disable all interrupts on this core - interrupt handlers might be in flash
    __asm volatile ("cpsid i" : : : "memory");
    
    while (1)
    {
        __asm volatile("nop");
    }
}

// Static disk controller reference for reset
static disk_controller_t* g_disk_controller = NULL;

void client_connected_cb(void)
{
    cpu_state_set_mode(CPU_RUNNING);
}

#if defined(VT100_DISPLAY) && defined(BLUETOOTH_KEYBOARD_SUPPORT)
static bool auto_started = false;
#endif

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

static uint32_t monotonic_ms(void)
{
    return to_ms_since_boot(get_absolute_time());
}

static uint8_t terminal_postprocess(uint8_t ch)
{
    ch &= ASCII_MASK_7BIT;
    if (ch == 28)
    {
        cpu_state_toggle_mode();
        return 0x00;
    }
    if (ch == '\n')
    {
        return '\r';
    }
    return ch;
}

// Terminal read function - non-blocking
static uint8_t terminal_read(void)
{

#if defined(CYW43_WL_GPIO_LED_PIN)
    // Input priority is WebSocket client, then BLE keyboard, then USB serial.
    uint8_t ws_ch = 0;
    if (websocket_console_has_client() && websocket_console_try_dequeue_input(&ws_ch))
    {
        uint8_t ch = ansi_input_process((uint8_t)(ws_ch & ASCII_MASK_7BIT), monotonic_ms());
        return terminal_postprocess(ch);
    }

#if defined(BLUETOOTH_KEYBOARD_SUPPORT)
    uint8_t bt_ch = 0;
    if (bt_keyboard_try_dequeue_input(&bt_ch))
    {
        uint8_t ch = ansi_input_process((uint8_t)(bt_ch & ASCII_MASK_7BIT), monotonic_ms());
        return terminal_postprocess(ch);
    }
#endif

    // Fall back to USB serial if neither higher-priority source has input.
    if (stdio_usb_connected())
    {
        int c = getchar_timeout_us(0);
        if (c != PICO_ERROR_TIMEOUT)
        {
            uint8_t ch = ansi_input_process((uint8_t)(c & ASCII_MASK_7BIT), monotonic_ms());
            return terminal_postprocess(ch);
        }
    }

    return terminal_postprocess(ansi_input_process(0x00, monotonic_ms()));
#else
    int c = getchar_timeout_us(0); // Non-blocking read
    if (c == PICO_ERROR_TIMEOUT)
    {
        return terminal_postprocess(ansi_input_process(0x00, monotonic_ms()));
    }

    uint8_t ch = (uint8_t)(c & ASCII_MASK_7BIT);
    ch = ansi_input_process(ch, monotonic_ms());
    return terminal_postprocess(ch);
#endif
}

// Terminal write function
static void terminal_write(uint8_t c)
{
    c &= ASCII_MASK_7BIT; // Take first 7 bits only
#if defined(CYW43_WL_GPIO_LED_PIN)
    websocket_console_enqueue_output(c);
    // Mirror output to USB serial if connected
    if (stdio_usb_connected())
    {
        putchar(c);
    }
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
    // Check for stored WiFi credentials and offer option to clear
    if (config_exists())
    {
        printf("\nWiFi credentials found in flash storage.\n");
        
        if (stdio_usb_connected())
        {
            printf("Press 'C' within 5 seconds to clear config and enter AP mode...\n");
            
            absolute_time_t start_time = get_absolute_time();
            while (absolute_time_diff_us(start_time, get_absolute_time()) < 5000000)
            {
                int c = getchar_timeout_us(100000); // Check every 100ms
                if (c == 'C' || c == 'c')
                {
                    printf("\nClearing WiFi configuration...\n");
                    config_clear();
                    printf("Config cleared. Rebooting into AP mode...\n");
                    sleep_ms(1000);
                    // Reboot the device
                    watchdog_enable(1, 1);
                    while (1) tight_loop_contents();
                }
            }
            printf("\n");
        }
    }
    else
    {
        printf("\nNo WiFi credentials configured - starting captive portal\n");
    }

#if defined(REMOTE_FS_SUPPORT) && !defined(SD_CARD_SUPPORT)
    // Initialize remote FS client queues BEFORE Core 1 starts polling
    rfs_client_init();
#endif

#if defined(BLUETOOTH_KEYBOARD_SUPPORT)
    bt_keyboard_queue_init();
#endif

    // Launch network task on core 1 (handles Wi-Fi init, WebSocket server, polling)
    websocket_console_start();

    // Wait for core 1 to complete Wi-Fi initialization
    // Returns 0 on failure, raw 32-bit IP address on success, or 0xFFFFFFFF for AP mode
    printf("Waiting for Wi-Fi initialization on core 1...\n");
    uint32_t ip_raw = wait_for_wifi();
    
    // Check for AP mode signal - spin in RAM while captive portal runs
    // This keeps Core 0 safe during flash operations (not executing from flash)
    if (ip_raw == 0xFFFFFFFF)
    {
        printf("AP mode active - Core 0 spinning while captive portal runs...\n");
        printf("Connect to 'Altair8800-Setup' WiFi to configure.\n");
        spin_in_ram(); // Never returns - device reboots after config save
    }

#if defined(BLUETOOTH_KEYBOARD_SUPPORT)
    // Enable multicore lockout on core 0 so core 1 can safely write to flash
    // (required for BTstack TLV bond storage via flash_safe_execute).
    // Must be AFTER wait_for_wifi() because the lockout IRQ handler consumes
    // all FIFO messages, which would swallow the IP address signal from core 1.
    flash_safe_execute_core_init();
#endif
    
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

#if defined(BLUETOOTH_KEYBOARD_SUPPORT)
    maybe_run_bluetooth_keyboard_shell();
#endif
}

#if defined(BLUETOOTH_KEYBOARD_SUPPORT)
static int serial_wait_for_char_ms(uint32_t timeout_ms)
{
    absolute_time_t start_time = get_absolute_time();
    while (absolute_time_diff_us(start_time, get_absolute_time()) < ((int64_t)timeout_ms * 1000))
    {
        int c = getchar_timeout_us(100000);
        if (c != PICO_ERROR_TIMEOUT)
        {
            return c;
        }
    }
    return PICO_ERROR_TIMEOUT;
}

static void print_bluetooth_keyboard_status(void)
{
    printf("Bluetooth keyboard: ");
    if (!bt_keyboard_is_ready())
    {
        printf("initializing");
    }
    else if (bt_keyboard_is_connected())
    {
        printf("connected");
    }
    else if (bt_keyboard_has_bond())
    {
        printf("bonded, waiting to reconnect");
    }
    else
    {
        printf("not paired");
    }
    printf("\n");
}

static void maybe_run_bluetooth_keyboard_shell(void)
{
    if (!stdio_usb_connected())
    {
        return;
    }

    print_bluetooth_keyboard_status();
    printf("Press 'B' within 5 seconds to manage Bluetooth keyboard pairing.\n");

    int c = serial_wait_for_char_ms(5000);
    if (c == PICO_ERROR_TIMEOUT)
    {
        return;
    }

    if (c != 'B' && c != 'b' && c != 'U' && c != 'u')
    {
        return;
    }

    if (c == 'U' || c == 'u')
    {
        bt_keyboard_request_clear_bonds();
    }

    printf("\nBluetooth keyboard manager\n");
    printf("  P - pair with BLE keyboard\n");
    printf("  U - clear stored Bluetooth bond\n");
    printf("  D - disconnect current keyboard\n");
    printf("  S - show status\n");
    printf("  Q - continue boot\n");

    for (;;)
    {
        // Auto-exit once a BLE keyboard connects (the shell reads from
        // USB stdio, so BLE keyboard input can't reach us here).
        if (bt_keyboard_is_connected())
        {
            printf("Keyboard connected, continuing boot.\n\n");
            return;
        }

        printf("bt> ");
        int cmd = PICO_ERROR_TIMEOUT;
        while (cmd == PICO_ERROR_TIMEOUT)
        {
            if (bt_keyboard_is_connected())
            {
                printf("\nKeyboard connected, continuing boot.\n\n");
                return;
            }
            cmd = getchar_timeout_us(100000);
        }

        if (cmd == '\r' || cmd == '\n')
        {
            continue;
        }

        switch (cmd)
        {
            case 'P':
            case 'p':
                printf("Starting BLE keyboard pairing flow...\n");
                printf("Select an empty Bluetooth slot on the keyboard, then hold the pairing key.\n");
                bt_keyboard_request_pairing();
                break;

            case 'U':
            case 'u':
                bt_keyboard_request_clear_bonds();
                break;

            case 'D':
            case 'd':
                bt_keyboard_request_disconnect();
                break;

            case 'S':
            case 's':
                print_bluetooth_keyboard_status();
                break;

            case 'Q':
            case 'q':
                printf("Leaving Bluetooth keyboard manager.\n\n");
                return;

            default:
                printf("Unknown command '%c'. Use P, U, D, S, or Q.\n", (char)cmd);
                break;
        }
    }
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

    // Note: Display initialization moved to Core 1

#if defined(CYW43_WL_GPIO_LED_PIN)
    // Board has WiFi - check if credentials exist
    config_init();

    if (!config_exists())
    {
        // No credentials - start AP mode immediately (do not block on USB serial)
        // Give USB a brief moment to enumerate if connected, then proceed
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

#if !(defined(WAVESHARE_3_5_DISPLAY) && defined(SD_CARD_SUPPORT))
    setup_wifi();
#endif
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
#if defined(CYW43_WL_GPIO_LED_PIN)
    if (g_wifi_ok)
    {
        printf("  URL: http://%s:8088\n", g_ip_buffer);
        printf("       http://%s:8088\n", wifi_get_hostname());
    }
#endif
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

    static FATFS fs;
#ifdef WAVESHARE_3_5_DISPLAY
    FRESULT fr = ws_f_mount(&fs, "", 1); // Immediate mount (calls disk_initialize internally)
#else
    FRESULT fr = f_mount(&fs, "", 1); // Immediate mount (calls disk_initialize internally)
#endif

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
#if defined(CYW43_WL_GPIO_LED_PIN) && defined(WAVESHARE_3_5_DISPLAY) && defined(SD_CARD_SUPPORT)
    printf("Deferring Wi-Fi/display startup until SD boot images are loaded on shared SPI1...\n");
    setup_wifi();
#endif
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
    uint32_t flash_used = (uint32_t)&__flash_binary_end - XIP_BASE;

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
    // Update Inky display with system information (if enabled)
    // Note: ST7789 display is updated on Core 1 after WiFi connects
    const char* wifi_ssid = g_wifi_ok ? get_connected_ssid() : NULL;
    inky_display_update(wifi_ssid, g_wifi_ok ? g_ip_buffer : NULL);
#else
    // No WiFi on this board
    inky_display_update(NULL, NULL);
#ifndef DISPLAY_ST7789_SUPPORT
    display_st7789_update(NULL, NULL);
#endif
#endif

    // ============================================

#if defined(VT100_DISPLAY) && defined(BLUETOOTH_KEYBOARD_SUPPORT)
    // With VT100 display + BT keyboard, start emulator immediately
    // instead of waiting for a WebSocket client to connect.
    if (!auto_started) {
        auto_started = true;
        cpu_state_set_mode(CPU_RUNNING);
        printf("Auto-starting emulator (VT100 + BT keyboard mode)\n");
    }
#endif

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
                bool handled = false;
                uint8_t ch = 0;
                if (websocket_console_try_dequeue_monitor_input(&ch))
                {
                    uint8_t filtered = ansi_input_process((uint8_t)(ch & ASCII_MASK_7BIT), monotonic_ms());
                    filtered = terminal_postprocess(filtered);
                    handled = true;
                    if (filtered != 0x00)
                    {
                        process_control_panel_commands_char(filtered);
                    }
                }

#if defined(BLUETOOTH_KEYBOARD_SUPPORT)
                if (!handled && bt_keyboard_try_dequeue_input(&ch))
                {
                    uint8_t filtered = ansi_input_process((uint8_t)(ch & ASCII_MASK_7BIT), monotonic_ms());
                    filtered = terminal_postprocess(filtered);
                    handled = true;
                    if (filtered != 0x00)
                    {
                        process_control_panel_commands_char(filtered);
                    }
                }
#endif

                // Check USB serial last for monitor commands.
                if (!handled && stdio_usb_connected())
                {
                    int c = getchar_timeout_us(0);
                    if (c != PICO_ERROR_TIMEOUT)
                    {
                        uint8_t sc = ansi_input_process((uint8_t)(c & ASCII_MASK_7BIT), monotonic_ms());
                        sc = terminal_postprocess(sc);
                        if (sc != 0x00)
                        {
                            process_control_panel_commands_char(sc);
                        }
                    }
                }

                if (!handled)
                {
                    uint8_t filtered = ansi_input_process(0x00, monotonic_ms());
                    filtered = terminal_postprocess(filtered);
                    if (filtered != 0x00)
                    {
                        process_control_panel_commands_char(filtered);
                    }
                }
            }
            break;
            default:
                tight_loop_contents(); // hint to compiler; no-op but lowers power
                break;
        }
    }
}
