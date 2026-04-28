/* Pico Display 2.8" LCD Support for Altair 8800 Emulator
 * Displays Altair front panel LEDs using custom async ST7789 driver
 * OPTIMIZED: Labels drawn once at init, only LEDs updated each frame
 */

#include "display_st7789.h"

#ifdef DISPLAY_ST7789_SUPPORT

#include "build_version.h"
#include "st7789_async.h"
#include "wifi.h"
#include <stdio.h>
#include <string.h>

// Layout constants
static const int LED_SIZE = 15;
static const int LED_SPACING_STATUS = 32;
static const int LED_SPACING_ADDRESS = 20;
static const int LED_SPACING_DATA = 20;

void display_st7789_init(void)
{
    // Initialize async ST7789 driver
    if (!st7789_async_init())
    {
        printf("Failed to initialize async ST7789 driver\n");
    }
}

void display_st7789_update(const char* ssid, const char* ip)
{
#ifdef CYW43_WL_GPIO_LED_PIN
    color_t TEXT_WHITE = rgb332(255, 255, 255);
    color_t BG_BLACK = rgb332(0, 0, 0);

    // Clear the WiFi info area (bottom right)
    st7789_async_fill_rect(0, 210, 320, 30, BG_BLACK);

    if (ip != NULL && ip[0] != '\0')
    {
        char ip_text[32];
        snprintf(ip_text, sizeof(ip_text), "HTTP://%s:8088", ip);

        // Calculate width: 5x8 font + 1px spacing = 6px per char
        int title_len = strlen(ip_text);
        int title_x = 320 - (title_len * 6) - 2;

        st7789_async_text(ip_text, title_x, 220, TEXT_WHITE);
        st7789_async_update();
        printf("[Display] WiFi info updated: %s\n", ip_text);
    }
#else
    (void)ssid;
    (void)ip;
#endif
}

void display_st7789_set_cpu_led(bool cpu_running)
{
    // RGB LED not implemented in async driver
}

void display_st7789_init_front_panel(void)
{
    // Clear screen once
    st7789_async_clear(rgb332(0, 0, 0));

    color_t TEXT_WHITE = rgb332(255, 255, 255);
    color_t TEXT_GRAY = rgb332(200, 200, 200);

    // === Draw all static elements ONCE ===

    // STATUS section - label and separator
    int y_status = 35;
    st7789_async_text("STATUS", 282, y_status - 15, TEXT_WHITE);
    st7789_async_fill_rect(0, y_status - 5, 320, 3, TEXT_WHITE);

    // Status labels (drawn once, never cleared)
    const char* status_labels[] = {"INT ", "WO  ", "STCK", "HLTA", "OUT ", "M1  ", "INP ", "MEMR", "PROT", "INTE"};
    int x_status = 8;
    // Status labels: 9..0 (INTE..INT)
    for (int i = 9; i >= 0; i--)
    {
        st7789_async_text(status_labels[i], x_status, y_status + LED_SIZE + 2, TEXT_GRAY);
        x_status += LED_SPACING_STATUS;
    }

    // ADDRESS section - label and separator
    int y_addr = 100;
    st7789_async_text("ADDRESS", 276, y_addr - 15, TEXT_WHITE);
    st7789_async_fill_rect(0, y_addr - 5, 320, 3, TEXT_WHITE);

    // Address labels (A15-A0)
    int x_addr = 3;
    for (int i = 15; i >= 0; i--)
    {
        char label[4];
        // Draw MSB (A15) at Left (first iteration), down to LSB (A0) at Right
        int label_bit = i;
        if (label_bit >= 10)
        {
            // label[0] = 'A';
            label[0] = '1';
            label[1] = '0' + (label_bit - 10);
            label[2] = '\0';
        }
        else
        {
            label[0] = ' ';
            label[1] = '0' + label_bit;
            label[2] = '\0';
        }
        st7789_async_text(label, x_addr, y_addr + LED_SIZE + 2, TEXT_GRAY);
        x_addr += LED_SPACING_ADDRESS;
    }

    // DATA section - label and separator
    int y_data = 170;
    st7789_async_text("DATA", 294, y_data - 15, TEXT_WHITE);
    st7789_async_fill_rect(0, y_data - 5, 320, 3, TEXT_WHITE);

    // Data labels (D7-D0)
    int x_data = 169;
    for (int i = 7; i >= 0; i--) // 7..0 (D7..D0)
    {
        char label[3];
        // Draw MSB (D7) at Left, down to LSB (D0) at Right
        int label_bit = i;
        // label[0] = 'D';
        label[0] = '0' + label_bit;
        label[1] = '\0';
        st7789_async_text(label, x_data, y_data + LED_SIZE + 2, TEXT_GRAY);
        x_data += LED_SPACING_DATA;
    }

    // Bottom section - WiFi IP (if available)
#ifdef CYW43_WL_GPIO_LED_PIN
    const char* ip = wifi_get_ip_address();
    if (ip != NULL)
    {
        char ip_text[32];
        snprintf(ip_text, sizeof(ip_text), "WIFI: %s:8088", ip);

        // Calculate width: 5x8 font + 1px spacing = 6px per char
        int title_len = strlen(ip_text);
        int title_x = 320 - (title_len * 6) - 2;

        st7789_async_text(ip_text, title_x, 220, TEXT_WHITE);
    }
#endif

    // Altair 8800 logo with build info
    char title_buffer[64];
    snprintf(title_buffer, sizeof(title_buffer), "ALTAIR 8800 (%d %s %s)", BUILD_VERSION, BUILD_DATE, BUILD_TIME);
    st7789_async_text(title_buffer, 2, 20, TEXT_WHITE);

    // Send initial frame
    st7789_async_update();

    printf("[Display] Static elements drawn (labels persist)\n");
}

void display_st7789_show_front_panel(uint16_t address, uint8_t data, uint16_t status)
{
    static uint16_t last_status = 0xFFFF; // Initialize to impossible values to force first draw
    static uint16_t last_address = 0xFFFF;
    static uint8_t last_data = 0xFF;

    bool needs_update = false;

    color_t LED_ON = rgb332(255, 0, 0); // Bright red
    color_t LED_OFF = rgb332(40, 0, 0); // Dark red

    // === ONLY update LEDs that actually changed (bit-level change detection) ===

    // STATUS LEDs (10 LEDs) - only update changed bits
    if (status != last_status)
    {
        uint16_t changed = status ^ last_status;
        int x_status = 8;
        int y_status = 35;
        for (int i = 9; i >= 0; i--)
        {
            if (changed & (1 << i))  // Only redraw if this bit changed
            {
                bool led_state = (status >> i) & 1;
                st7789_async_fill_rect(x_status, y_status, LED_SIZE, LED_SIZE, led_state ? LED_ON : LED_OFF);
            }
            x_status += LED_SPACING_STATUS;
        }
        last_status = status;
        needs_update = true;
    }

    // ADDRESS LEDs (16 LEDs) - only update changed bits
    if (address != last_address)
    {
        uint16_t changed = address ^ last_address;
        int x_addr = 2;
        int y_addr = 100;
        for (int i = 15; i >= 0; i--)
        {
            if (changed & (1 << i))  // Only redraw if this bit changed
            {
                bool led_state = (address >> i) & 1;
                st7789_async_fill_rect(x_addr, y_addr, LED_SIZE, LED_SIZE, led_state ? LED_ON : LED_OFF);
            }
            x_addr += LED_SPACING_ADDRESS;
        }
        last_address = address;
        needs_update = true;
    }

    // DATA LEDs (8 LEDs) - only update changed bits
    if (data != last_data)
    {
        uint8_t changed = data ^ last_data;
        int x_data = 162;
        int y_data = 170;
        for (int i = 7; i >= 0; i--)
        {
            if (changed & (1 << i))  // Only redraw if this bit changed
            {
                bool led_state = (data >> i) & 1;
                st7789_async_fill_rect(x_data, y_data, LED_SIZE, LED_SIZE, led_state ? LED_ON : LED_OFF);
            }
            x_data += LED_SPACING_DATA;
        }
        last_data = data;
        needs_update = true;
    }

    // Only send update to display if something changed
    if (needs_update)
    {
        st7789_async_update();
    }
}

void display_st7789_get_stats(uint64_t* skipped_updates)
{
    uint64_t updates, skipped;
    st7789_async_get_stats(&updates, &skipped);
    if (skipped_updates)
    {
        *skipped_updates = skipped;
    }
}

#endif // DISPLAY_ST7789_SUPPORT
