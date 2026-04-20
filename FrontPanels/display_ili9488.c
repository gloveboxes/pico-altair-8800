/* Waveshare 3.5" ILI9488 LCD Support for Altair 8800 Emulator
 * Composes the entire LED+label panel area off-screen and updates it with
 * one DMA transaction when values change.
 */

#include "display_ili9488.h"

#ifdef WAVESHARE_3_5_DISPLAY

#include "build_version.h"
#include "font_5x8.h"
#include "ili9488_async.h"
#include <stdio.h>
#include <string.h>

#ifdef CYW43_WL_GPIO_LED_PIN
#include "wifi.h"
#endif

// Layout constants for 480x320 display
static const int LED_SIZE = 18;

static const int LED_SPACING_STATUS = 46;
static const int STATUS_X_START = 12;

static const int LED_SPACING_ADDRESS = 29;
static const int ADDRESS_X_START = 6;

static const int LED_SPACING_DATA = 29;
static const int DATA_X_START = 238;

static const int Y_TITLE = 10;
static const int Y_STATUS_LABEL = 45;
static const int Y_STATUS_SEP = 64;
static const int Y_STATUS_LEDS = 72;
static const int Y_STATUS_NAMES = 94;

static const int Y_ADDR_LABEL = 110;
static const int Y_ADDR_SEP = 129;
static const int Y_ADDR_LEDS = 137;
static const int Y_ADDR_NAMES = 159;

static const int Y_DATA_LABEL = 180;
static const int Y_DATA_SEP = 199;
static const int Y_DATA_LEDS = 207;
static const int Y_DATA_NAMES = 229;

static const int Y_WIFI_INFO = 290;
static const int RIGHT_MARGIN = 20;

// Full update area (section headers, separators, LED rows, bit labels)
#define PANEL_AREA_X 0
#define PANEL_AREA_Y 45
#define PANEL_AREA_W ILI9488_WIDTH
#define PANEL_AREA_H 192

static ili9488_color_t g_panel_bitmap[PANEL_AREA_W * PANEL_AREA_H];

static inline int to_local_y(int y)
{
    return y - PANEL_AREA_Y;
}

static inline int panel_index(int x, int y)
{
    return y * PANEL_AREA_W + x;
}

static void panel_clear(ili9488_color_t color)
{
    for (int i = 0; i < PANEL_AREA_W * PANEL_AREA_H; i++)
    {
        g_panel_bitmap[i] = color;
    }
}

static void panel_fill_rect(int x, int y, int w, int h, ili9488_color_t color)
{
    if (x < 0)
    {
        w += x;
        x = 0;
    }
    if (y < 0)
    {
        h += y;
        y = 0;
    }
    if (x + w > PANEL_AREA_W)
        w = PANEL_AREA_W - x;
    if (y + h > PANEL_AREA_H)
        h = PANEL_AREA_H - y;
    if (w <= 0 || h <= 0)
        return;

    for (int row = 0; row < h; row++)
    {
        int base = panel_index(x, y + row);
        for (int col = 0; col < w; col++)
        {
            g_panel_bitmap[base + col] = color;
        }
    }
}

static int char_to_glyph_idx(char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a';
    if (c >= '0' && c <= '9')
        return 26 + (c - '0');
    if (c == '.')
        return 36;
    if (c == '-')
        return 37;
    if (c == ':')
        return 38;
    if (c == '(')
        return 39;
    if (c == ')')
        return 40;
    if (c == '/')
        return 41;
    return -1;
}

static void panel_text_1x(const char* str, int x, int y, ili9488_color_t fg, ili9488_color_t bg)
{
    int ly = to_local_y(y);

    while (*str)
    {
        char c = *str++;
        if (c == ' ')
        {
            x += 6;
            continue;
        }

        int glyph_idx = char_to_glyph_idx(c);
        if (glyph_idx < 0 || glyph_idx >= FONT_GLYPH_COUNT)
        {
            x += 6;
            continue;
        }

        for (int row = 0; row < 8; row++)
        {
            int py = ly + row;
            if (py < 0 || py >= PANEL_AREA_H)
                continue;

            for (int col = 0; col < 6; col++)
            {
                int px = x + col;
                if (px < 0 || px >= PANEL_AREA_W)
                    continue;

                ili9488_color_t color = bg;
                if (col < 5)
                {
                    uint8_t column_data = font_5x8[glyph_idx][col];
                    color = (column_data & (1 << row)) ? fg : bg;
                }
                g_panel_bitmap[panel_index(px, py)] = color;
            }
        }

        x += 6;
    }
}

static void panel_text_2x(const char* str, int x, int y, ili9488_color_t fg, ili9488_color_t bg)
{
    int ly = to_local_y(y);

    while (*str)
    {
        char c = *str++;
        if (c == ' ')
        {
            x += 12;
            continue;
        }

        int glyph_idx = char_to_glyph_idx(c);
        if (glyph_idx < 0 || glyph_idx >= FONT_GLYPH_COUNT)
        {
            x += 12;
            continue;
        }

        for (int row = 0; row < 16; row++)
        {
            int py = ly + row;
            if (py < 0 || py >= PANEL_AREA_H)
                continue;

            int src_row = row / 2;
            for (int col = 0; col < 12; col++)
            {
                int px = x + col;
                if (px < 0 || px >= PANEL_AREA_W)
                    continue;

                int src_col = col / 2;
                ili9488_color_t color = bg;
                if (src_col < 5)
                {
                    uint8_t column_data = font_5x8[glyph_idx][src_col];
                    color = (column_data & (1 << src_row)) ? fg : bg;
                }
                g_panel_bitmap[panel_index(px, py)] = color;
            }
        }

        x += 12;
    }
}

static void panel_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                               int spacing, ili9488_color_t on_color, ili9488_color_t off_color)
{
    int ly = to_local_y(y);
    int x = x_start;

    for (int i = num_leds - 1; i >= 0; i--)
    {
        bool on = (bits >> i) & 1U;
        panel_fill_rect(x, ly, LED_SIZE, LED_SIZE, on ? on_color : off_color);
        x += spacing;
    }
}

static void compose_panel_bitmap(uint16_t address, uint8_t data, uint16_t status)
{
    // Classic Altair 8800 / MITS blue front-panel theme
    ili9488_color_t BG_BLACK = ili9488_rgb565(25, 55, 110);
    ili9488_color_t TEXT_WHITE = ili9488_rgb565(255, 255, 255);
    ili9488_color_t TEXT_GRAY = ili9488_rgb565(180, 200, 220);
    ili9488_color_t SEP_COLOR = ili9488_rgb565(200, 210, 220);
    ili9488_color_t LED_ON = ili9488_rgb565(255, 0, 0);
    ili9488_color_t LED_OFF = ili9488_rgb565(50, 20, 30);

    panel_clear(BG_BLACK);

    // Section headers and separators
    panel_text_2x("STATUS", ILI9488_WIDTH - (6 * 12) - RIGHT_MARGIN, Y_STATUS_LABEL, TEXT_WHITE, BG_BLACK);
    panel_fill_rect(0, to_local_y(Y_STATUS_SEP), ILI9488_WIDTH, 3, SEP_COLOR);

    panel_text_2x("ADDRESS", ILI9488_WIDTH - (7 * 12) - RIGHT_MARGIN, Y_ADDR_LABEL, TEXT_WHITE, BG_BLACK);
    panel_fill_rect(0, to_local_y(Y_ADDR_SEP), ILI9488_WIDTH, 3, SEP_COLOR);

    panel_text_2x("DATA", ILI9488_WIDTH - (4 * 12) - RIGHT_MARGIN, Y_DATA_LABEL, TEXT_WHITE, BG_BLACK);
    panel_fill_rect(0, to_local_y(Y_DATA_SEP), ILI9488_WIDTH, 3, SEP_COLOR);

    // Static LED bit labels
    const char* status_labels[] = {
        "INT ", "WO  ", "STCK", "HLTA", "OUT ",
        "M1  ", "INP ", "MEMR", "PROT", "INTE"};

    int x = STATUS_X_START;
    for (int i = 9; i >= 0; i--)
    {
        int text_w = (int)strlen(status_labels[i]) * 6;
        int tx = x + (LED_SIZE - text_w) / 2;
        panel_text_1x(status_labels[i], tx, Y_STATUS_NAMES, TEXT_GRAY, BG_BLACK);
        x += LED_SPACING_STATUS;
    }

    x = ADDRESS_X_START;
    for (int i = 15; i >= 0; i--)
    {
        char label[3];
        if (i >= 10)
        {
            label[0] = '1';
            label[1] = '0' + (i - 10);
            label[2] = '\0';
        }
        else
        {
            label[0] = '0' + i;
            label[1] = '\0';
        }
        panel_text_1x(label, x + 4, Y_ADDR_NAMES, TEXT_GRAY, BG_BLACK);
        x += LED_SPACING_ADDRESS;
    }

    x = DATA_X_START;
    for (int i = 7; i >= 0; i--)
    {
        char label[2];
        label[0] = '0' + i;
        label[1] = '\0';
        panel_text_1x(label, x + 5, Y_DATA_NAMES, TEXT_GRAY, BG_BLACK);
        x += LED_SPACING_DATA;
    }

    // Dynamic LED rows
    panel_draw_led_row(status, 10, STATUS_X_START, Y_STATUS_LEDS,
                       LED_SPACING_STATUS, LED_ON, LED_OFF);
    panel_draw_led_row(address, 16, ADDRESS_X_START, Y_ADDR_LEDS,
                       LED_SPACING_ADDRESS, LED_ON, LED_OFF);
    panel_draw_led_row(data, 8, DATA_X_START, Y_DATA_LEDS,
                       LED_SPACING_DATA, LED_ON, LED_OFF);
}

void display_ili9488_init(void)
{
    if (!ili9488_async_init())
    {
        printf("[ILI9488] Failed to initialize display driver\n");
        return;
    }
    printf("[ILI9488] Display initialized (480x320, 62.5MHz SPI, DMA)\n");
}

void display_ili9488_update(const char* ssid, const char* ip)
{
#ifdef CYW43_WL_GPIO_LED_PIN
    ili9488_color_t TEXT_WHITE = ili9488_rgb565(255, 255, 255);
    ili9488_color_t BG_BLUE = ili9488_rgb565(25, 55, 110);

    (void)ssid;

    // Clear the WiFi info area
    ili9488_async_fill_rect(0, Y_WIFI_INFO - 2, ILI9488_WIDTH, 18, BG_BLUE);

    if (ip != NULL && ip[0] != '\0')
    {
        char ip_text[96];
        const char* host = wifi_get_hostname();
        if (host && host[0] != '\0')
            snprintf(ip_text, sizeof(ip_text), "WIFI: %s | %s.local", ip, host);
        else
            snprintf(ip_text, sizeof(ip_text), "WIFI: %s", ip);

        ili9488_async_text(ip_text, 6, Y_WIFI_INFO, TEXT_WHITE, BG_BLUE);
    }
#else
    (void)ssid;
    (void)ip;
#endif
}

void display_ili9488_init_front_panel(void)
{
    ili9488_color_t TEXT_WHITE = ili9488_rgb565(255, 255, 255);

    // Clear entire screen with classic Altair 8800 MITS blue
    ili9488_async_clear(ili9488_rgb565(25, 55, 110));

    // Title
    char title_buffer[64];
    snprintf(title_buffer, sizeof(title_buffer), "ALTAIR 8800 (%d %s %s)",
             BUILD_VERSION, BUILD_DATE, BUILD_TIME);
    ili9488_color_t BG_BLUE = ili9488_rgb565(25, 55, 110);
    ili9488_async_text_2x(title_buffer, 4, Y_TITLE, TEXT_WHITE, BG_BLUE);

    // Initial WiFi line if available
#ifdef CYW43_WL_GPIO_LED_PIN
    const char* ip = wifi_get_ip_address();
    if (ip != NULL && ip[0] != '\0')
    {
        char ip_text[96];
        const char* host = wifi_get_hostname();
        if (host && host[0] != '\0')
            snprintf(ip_text, sizeof(ip_text), "WIFI: %s | %s.local", ip, host);
        else
            snprintf(ip_text, sizeof(ip_text), "WIFI: %s", ip);
        ili9488_async_text(ip_text, 6, Y_WIFI_INFO, TEXT_WHITE, BG_BLUE);
    }
#endif

    // Force an initial full panel draw with one-area composition path
    compose_panel_bitmap(0, 0, 0);
    ili9488_async_blit_rgb565(PANEL_AREA_X, PANEL_AREA_Y, PANEL_AREA_W, PANEL_AREA_H, g_panel_bitmap);

    printf("[ILI9488] Front panel initialized with one-shot DMA area updates\n");
}

void display_ili9488_show_front_panel(uint16_t address, uint8_t data, uint16_t status)
{
    static uint16_t last_status = 0xFFFF;
    static uint16_t last_address = 0xFFFF;
    static uint8_t last_data = 0xFF;

    // Progress pending DMA transfers/queue without blocking.
    ili9488_async_service();

    if (status == last_status && address == last_address && data == last_data)
    {
        return;
    }

    // Single-buffer mode: only compose when no in-flight DMA uses this buffer.
    if (!ili9488_async_is_ready())
    {
        return;
    }

    compose_panel_bitmap(address, data, status);

    if (!ili9488_async_blit_rgb565_async(PANEL_AREA_X, PANEL_AREA_Y, PANEL_AREA_W, PANEL_AREA_H, g_panel_bitmap))
    {
        return;
    }

    last_status = status;
    last_address = address;
    last_data = data;
}

void display_ili9488_poll(void)
{
    ili9488_async_service();
}

void display_ili9488_get_stats(uint64_t* skipped_updates)
{
    uint64_t updates, completions;
    ili9488_async_get_stats(&updates, &completions);
    if (skipped_updates)
    {
        *skipped_updates = 0;
    }
}

#endif // WAVESHARE_3_5_DISPLAY
