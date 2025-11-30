#include "pico/stdlib.h"
#include "pico/error.h"
#include <stdio.h>
#include <string.h>
#include "Altair8800/intel8080.h"
#include "Altair8800/memory.h"
#include "Altair8800/pico_disk.h"
#include "io_ports.h"
#include "websocket_console.h"

#define ASCII_MASK_7BIT 0x7F
#define CTRL_KEY(ch) ((ch) & 0x1F)

// Include the CPM disk image
#include "Altair8800/cpm63k_disk.h"

// Global CPU instance
static intel8080_t cpu;

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
    uint8_t ws_ch = 0;
    if (websocket_console_try_dequeue_input(&ws_ch))
    {
        return (uint8_t)(ws_ch & ASCII_MASK_7BIT);
    }

    int c = getchar_timeout_us(0); // Non-blocking read
    if (c == PICO_ERROR_TIMEOUT)
    {
        return 0x00; // Return null if no character available
    }

    uint8_t ch = (uint8_t)(c & ASCII_MASK_7BIT);
    return process_ansi_sequence(ch);
}

// Terminal write function
static void terminal_write(uint8_t c)
{
    c &= ASCII_MASK_7BIT; // Take first 7 bits only
    putchar(c);
    websocket_console_enqueue_output(c);
}

// Sense switches stub
static inline uint8_t sense(void)
{
    return 0x00; // No sense switches on Pico
}

int main(void)
{
    // Initialize stdio first
    stdio_init_all();

    // Give more time for USB serial to enumerate
    sleep_ms(3000);

    // Launch network task on core 1 (handles Wi-Fi init, WebSocket server, polling)
    websocket_console_start();

    // Wait for core 1 to complete Wi-Fi initialization
    // Returns 0 on failure, or raw 32-bit IP address on success
    printf("Waiting for Wi-Fi initialization on core 1...\n");
    uint32_t ip_raw = websocket_console_wait_for_wifi();
    char ip_buffer[32] = {0};
    bool wifi_ok = (ip_raw != 0);

    if (wifi_ok)
    {
        // Convert raw IP to dotted-decimal string
        snprintf(ip_buffer, sizeof(ip_buffer), "%lu.%lu.%lu.%lu",
                 (unsigned long)(ip_raw & 0xFF),
                 (unsigned long)((ip_raw >> 8) & 0xFF),
                 (unsigned long)((ip_raw >> 16) & 0xFF),
                 (unsigned long)((ip_raw >> 24) & 0xFF));
        printf("Wi-Fi connected. IP: %s\n", ip_buffer);
    }
    else
    {
        strncpy(ip_buffer, "No network", sizeof(ip_buffer) - 1);
        printf("Wi-Fi unavailable; USB terminal only.\n");
    }

    // Send test output
    printf("\n\n*** USB Serial Active ***\n");
    printf("========================================\n");
    printf("  Altair 8800 Emulator - Pico 2 W\n");
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

    // Load disk boot loader ROM at 0xFF00 (ROM_LOADER_ADDRESS)
    printf("Loading disk boot loader ROM at 0xFF00...\n");
    loadDiskLoader(0xFF00);

    // Set up disk controller structure for CPU
    disk_controller_t disk_controller = {
        .disk_select = (port_out)pico_disk_select,
        .disk_status = (port_in)pico_disk_status,
        .disk_function = (port_out)pico_disk_function,
        .sector = (port_in)pico_disk_sector,
        .write = (port_out)pico_disk_write,
        .read = (port_in)pico_disk_read};

    // Reset and initialize the CPU
    printf("Initializing Intel 8080 CPU...\n");
    i8080_reset(&cpu,
                terminal_read,
                terminal_write,
                sense,
                &disk_controller,
                io_port_in,
                io_port_out);

    // Set CPU to start at ROM_LOADER_ADDRESS (0xFF00) to boot from disk
    printf("Setting CPU to ROM_LOADER_ADDRESS (0xFF00) to boot from disk\n");
    i8080_examine(&cpu, 0xFF00);

    // Report memory usage
    extern char __StackLimit, __bss_end__;
    extern char __flash_binary_end;

    uint32_t heap_free = &__StackLimit - &__bss_end__;
    uint32_t total_ram = 512 * 1024; // Pico 2 W has 512KB SRAM
    uint32_t used_ram = total_ram - heap_free;
    uint32_t flash_used = (uint32_t)&__flash_binary_end;

    printf("\n");
    printf("Memory Report:\n");
    printf("  Flash used:     %lu bytes (%.1f KB)\n", flash_used, flash_used / 1024.0f);
    printf("  RAM used:       %lu bytes (%.1f KB)\n", used_ram, used_ram / 1024.0f);
    printf("  RAM free (heap):%lu bytes (%.1f KB)\n", heap_free, heap_free / 1024.0f);
    printf("  Total SRAM:     %lu bytes (512 KB)\n", total_ram);
    printf("  Altair memory:  65536 bytes (64 KB)\n");
    printf("\n");

    printf("Starting Altair 8800 emulation...\n");
    printf("\n");

    // Main emulation loop - core 0 dedicated to CPU emulation
    while (true)
    {
        i8080_cycle(&cpu);
    }
}
