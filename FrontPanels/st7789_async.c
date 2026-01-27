/* Custom Direct-Write ST7789 Driver for Pimoroni Pico Display 2.8"
 * Framebuffer-less implementation - writes directly to display
 * Saves ~150KB RAM by using partial window updates
 *
 * Pin configuration for Pimoroni Pico Display 2.8":
 * - DC: GPIO 16
 * - CS: GPIO 17
 * - SCK: GPIO 18 (SPI0)
 * - MOSI: GPIO 19 (SPI0)
 * - BL: GPIO 20
 * - RGB LED: GPIO 26, 27, 28
 */

#include "st7789_async.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include <string.h>

// ST7789 Commands
#define ST7789_SWRESET 0x01
#define ST7789_SLPOUT 0x11
#define ST7789_COLMOD 0x3A
#define ST7789_MADCTL 0x36
#define ST7789_CASET 0x2A
#define ST7789_RASET 0x2B
#define ST7789_RAMWR 0x2C
#define ST7789_DISPON 0x29
#define ST7789_INVON 0x21

// Pin definitions for Pimoroni Pico Display 2.8"
#define PIN_DC 16
#define PIN_CS 17
#define PIN_SCK 18
#define PIN_MOSI 19
#define PIN_BL 20
#define PIN_LED_R 26
#define PIN_LED_G 27
#define PIN_LED_B 28

// SPI instance - Pimoroni uses GPIO 18/19 which are SPI0
#define SPI_INST spi0

// Small buffer for rectangle fills (max LED size 15x15 = 225 pixels = 450 bytes)
#define RECT_BUFFER_SIZE 512
static uint16_t g_rect_buffer[RECT_BUFFER_SIZE];

// DMA channel for fast pixel transfers
static int g_dma_channel = -1;

// Statistics
static uint64_t g_update_count = 0;

// Minimal 5x8 font for capital letters, numbers, and period
static const uint8_t font_5x8[][5] = {
    {0x7E, 0x11, 0x11, 0x11, 0x7E}, // A
    {0x7F, 0x49, 0x49, 0x49, 0x36}, // B
    {0x3E, 0x41, 0x41, 0x41, 0x22}, // C
    {0x7F, 0x41, 0x41, 0x22, 0x1C}, // D
    {0x7F, 0x49, 0x49, 0x49, 0x41}, // E
    {0x7F, 0x09, 0x09, 0x09, 0x01}, // F
    {0x3E, 0x41, 0x49, 0x49, 0x7A}, // G
    {0x7F, 0x08, 0x08, 0x08, 0x7F}, // H
    {0x00, 0x41, 0x7F, 0x41, 0x00}, // I
    {0x20, 0x40, 0x41, 0x3F, 0x01}, // J
    {0x7F, 0x08, 0x14, 0x22, 0x41}, // K
    {0x7F, 0x40, 0x40, 0x40, 0x40}, // L
    {0x7F, 0x02, 0x0C, 0x02, 0x7F}, // M
    {0x7F, 0x04, 0x08, 0x10, 0x7F}, // N
    {0x3E, 0x41, 0x41, 0x41, 0x3E}, // O
    {0x7F, 0x09, 0x09, 0x09, 0x06}, // P
    {0x3E, 0x41, 0x51, 0x21, 0x5E}, // Q
    {0x7F, 0x09, 0x19, 0x29, 0x46}, // R
    {0x46, 0x49, 0x49, 0x49, 0x31}, // S
    {0x01, 0x01, 0x7F, 0x01, 0x01}, // T
    {0x3F, 0x40, 0x40, 0x40, 0x3F}, // U
    {0x1F, 0x20, 0x40, 0x20, 0x1F}, // V
    {0x3F, 0x40, 0x38, 0x40, 0x3F}, // W
    {0x63, 0x14, 0x08, 0x14, 0x63}, // X
    {0x07, 0x08, 0x70, 0x08, 0x07}, // Y
    {0x61, 0x51, 0x49, 0x45, 0x43}, // Z
    {0x3E, 0x51, 0x49, 0x45, 0x3E}, // 0
    {0x00, 0x42, 0x7F, 0x40, 0x00}, // 1
    {0x42, 0x61, 0x51, 0x49, 0x46}, // 2
    {0x21, 0x41, 0x45, 0x4B, 0x31}, // 3
    {0x18, 0x14, 0x12, 0x7F, 0x10}, // 4
    {0x27, 0x45, 0x45, 0x45, 0x39}, // 5
    {0x3C, 0x4A, 0x49, 0x49, 0x30}, // 6
    {0x01, 0x71, 0x09, 0x05, 0x03}, // 7
    {0x36, 0x49, 0x49, 0x49, 0x36}, // 8
    {0x06, 0x49, 0x49, 0x29, 0x1E}, // 9
    {0x00, 0x00, 0x60, 0x60, 0x00}, // . (period/dot) - index 36
    {0x08, 0x08, 0x08, 0x08, 0x08}, // - (hyphen) - index 37
    {0x00, 0x00, 0x36, 0x36, 0x00}, // : (colon) - index 38
    {0x00, 0x1C, 0x22, 0x41, 0x00}, // ( (left paren) - index 39
    {0x00, 0x41, 0x22, 0x1C, 0x00}, // ) (right paren) - index 40
};

// Wait for DMA to complete AND SPI to finish transmitting
static inline void wait_for_dma(void)
{
    if (g_dma_channel >= 0) {
        while (dma_channel_is_busy(g_dma_channel))
            tight_loop_contents();
        // Must wait for SPI FIFO to drain after DMA completes
        while (spi_is_busy(SPI_INST))
            tight_loop_contents();
    }
}

// Send command to display
static void send_command(uint8_t cmd)
{
    wait_for_dma();  // Ensure previous DMA complete
    gpio_put(PIN_DC, 0); // Command mode
    gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_INST, &cmd, 1);
    gpio_put(PIN_CS, 1);
}

// Send data to display (blocking)
static void send_data(const uint8_t* data, size_t len)
{
    wait_for_dma();  // Ensure previous DMA complete
    gpio_put(PIN_DC, 1); // Data mode
    gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_INST, data, len);
    gpio_put(PIN_CS, 1);
}

// Send data via DMA (faster for larger transfers)
static void send_data_dma(const uint8_t* data, size_t len)
{
    wait_for_dma();  // Ensure previous DMA complete
    gpio_put(PIN_DC, 1); // Data mode
    gpio_put(PIN_CS, 0);
    
    dma_channel_set_read_addr(g_dma_channel, data, false);
    dma_channel_set_trans_count(g_dma_channel, len, true);
    
    // Wait for this transfer to complete
    wait_for_dma();
    gpio_put(PIN_CS, 1);
}

// Set display window for pixel writing
static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    uint8_t data[4];

    // Column address set
    send_command(ST7789_CASET);
    data[0] = x0 >> 8;
    data[1] = x0 & 0xFF;
    data[2] = x1 >> 8;
    data[3] = x1 & 0xFF;
    send_data(data, 4);

    // Row address set
    send_command(ST7789_RASET);
    data[0] = y0 >> 8;
    data[1] = y0 & 0xFF;
    data[2] = y1 >> 8;
    data[3] = y1 & 0xFF;
    send_data(data, 4);
}

bool st7789_async_init(void)
{
    // Initialize SPI at 75 MHz
    spi_init(SPI_INST, 75 * 1000 * 1000);

    // Set up SPI pins
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // Set up control pins
    gpio_init(PIN_DC);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    // Set up DMA channel for fast pixel transfers
    g_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config dma_cfg = dma_channel_get_default_config(g_dma_channel);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&dma_cfg, spi_get_dreq(SPI_INST, true));
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    dma_channel_configure(g_dma_channel, &dma_cfg, &spi_get_hw(SPI_INST)->dr, NULL, 0, false);

    // Set up backlight (PWM)
    gpio_set_function(PIN_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_BL);
    pwm_set_wrap(slice, 65535);
    pwm_set_gpio_level(PIN_BL, 65535); // Full brightness
    pwm_set_enabled(slice, true);

    // Turn off RGB LED (active-low: HIGH = off, LOW = on)
    gpio_init(PIN_LED_R);
    gpio_set_dir(PIN_LED_R, GPIO_OUT);
    gpio_put(PIN_LED_R, 1);
    gpio_init(PIN_LED_G);
    gpio_set_dir(PIN_LED_G, GPIO_OUT);
    gpio_put(PIN_LED_G, 1);
    gpio_init(PIN_LED_B);
    gpio_set_dir(PIN_LED_B, GPIO_OUT);
    gpio_put(PIN_LED_B, 1);

    // Software reset
    send_command(ST7789_SWRESET);
    sleep_ms(150);

    // Sleep out
    send_command(ST7789_SLPOUT);
    sleep_ms(10);

    // Color mode: RGB565 (16-bit color)
    send_command(ST7789_COLMOD);
    uint8_t colmod = 0x05; // 16-bit/pixel
    send_data(&colmod, 1);

    // Memory access control for 320x240 (Rotated 180 from 0xA0)
    send_command(ST7789_MADCTL);
    uint8_t madctl = 0x60; // MX + MV (Landscape rotated 180)
    send_data(&madctl, 1);

    // Gamma control for 320x240 - from Pimoroni driver
    send_command(0xB7); // GCTRL
    uint8_t gctrl = 0x35;
    send_data(&gctrl, 1);

    send_command(0xBB); // VCOMS
    uint8_t vcoms = 0x1f;
    send_data(&vcoms, 1);

    // Inversion on (clearer colors)
    send_command(ST7789_INVON);

    // Display on
    send_command(ST7789_DISPON);
    sleep_ms(10);

    // Clear screen to black
    st7789_async_clear(0);

    return true;
}

// Direct text rendering - writes each character directly to display
void st7789_async_text(const char* str, int x, int y, color_t color)
{
    color_t bg_color = rgb565(0, 0, 0); // Black background
    
    while (*str)
    {
        char c = *str++;
        int glyph_idx = -1;

        // Map character to glyph index
        if (c >= 'A' && c <= 'Z')
            glyph_idx = c - 'A';
        else if (c >= '0' && c <= '9')
            glyph_idx = 26 + (c - '0');
        else if (c == '.')
            glyph_idx = 36;
        else if (c == '-')
            glyph_idx = 37;
        else if (c == ':')
            glyph_idx = 38;
        else if (c == '(')
            glyph_idx = 39;
        else if (c == ')')
            glyph_idx = 40;
        else if (c == ' ')
        {
            x += 6;
            continue;
        }

        if (glyph_idx >= 0 && glyph_idx < 41)
        {
            // Set window for this character (6x8 including spacing)
            set_window(x, y, x + 5, y + 7);
            send_command(ST7789_RAMWR);
            
            // Build character pixels into buffer (6 cols x 8 rows = 48 pixels)
            uint16_t char_buf[48];
            int idx = 0;
            for (int row = 0; row < 8; row++)
            {
                for (int col = 0; col < 6; col++)
                {
                    if (col < 5)
                    {
                        uint8_t column_data = font_5x8[glyph_idx][col];
                        char_buf[idx++] = (column_data & (1 << row)) ? color : bg_color;
                    }
                    else
                    {
                        char_buf[idx++] = bg_color; // Spacing column
                    }
                }
            }
            send_data((uint8_t*)char_buf, sizeof(char_buf));
            x += 6;
        }
    }
}

// Direct fill rectangle - writes directly to display using partial window with DMA
void st7789_async_fill_rect(int x, int y, int w, int h, color_t color)
{
    // Clamp to screen bounds
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
    if (x + w > ST7789_ASYNC_WIDTH)
        w = ST7789_ASYNC_WIDTH - x;
    if (y + h > ST7789_ASYNC_HEIGHT)
        h = ST7789_ASYNC_HEIGHT - y;
    if (w <= 0 || h <= 0)
        return;

    // Set window for this rectangle
    set_window(x, y, x + w - 1, y + h - 1);
    send_command(ST7789_RAMWR);

    int total_pixels = w * h;
    
    // Fill buffer with color
    int buf_pixels = (total_pixels < RECT_BUFFER_SIZE) ? total_pixels : RECT_BUFFER_SIZE;
    for (int i = 0; i < buf_pixels; i++)
    {
        g_rect_buffer[i] = color;
    }

    // Send data in chunks using DMA
    int pixels_sent = 0;
    while (pixels_sent < total_pixels)
    {
        int chunk = total_pixels - pixels_sent;
        if (chunk > buf_pixels)
            chunk = buf_pixels;
        send_data_dma((uint8_t*)g_rect_buffer, chunk * 2);
        pixels_sent += chunk;
    }
    
    g_update_count++;
}

// Draw a row of LEDs efficiently - only draws changed LEDs
void st7789_async_draw_led_row(uint32_t bits, int num_leds, int x_start, int y, 
                               int led_size, int spacing, color_t on_color, color_t off_color)
{
    // Fill buffer with LED pixels
    int led_pixels = led_size * led_size;
    if (led_pixels > RECT_BUFFER_SIZE)
        led_pixels = RECT_BUFFER_SIZE;
    
    // Draw each LED (bit 0 = rightmost)
    for (int i = 0; i < num_leds; i++)
    {
        int bit_idx = num_leds - 1 - i;  // Rightmost LED is bit 0
        bool on = (bits >> bit_idx) & 1;
        color_t color = on ? on_color : off_color;
        
        // Calculate LED position
        int x = x_start + i * spacing;
        
        // Fill buffer with this LED's color
        for (int p = 0; p < led_pixels; p++)
        {
            g_rect_buffer[p] = color;
        }
        
        // Set window and send via DMA
        set_window(x, y, x + led_size - 1, y + led_size - 1);
        send_command(ST7789_RAMWR);
        send_data_dma((uint8_t*)g_rect_buffer, led_pixels * 2);
    }
    
    g_update_count++;
}

// Clear entire screen - direct write with DMA
void st7789_async_clear(color_t color)
{
    // Set window to full screen
    set_window(0, 0, ST7789_ASYNC_WIDTH - 1, ST7789_ASYNC_HEIGHT - 1);
    send_command(ST7789_RAMWR);

    // Fill buffer with color
    for (int i = 0; i < RECT_BUFFER_SIZE; i++)
    {
        g_rect_buffer[i] = color;
    }

    // Send full screen worth of data (320 * 240 = 76800 pixels) using DMA
    int total_pixels = ST7789_ASYNC_WIDTH * ST7789_ASYNC_HEIGHT;
    int pixels_sent = 0;
    while (pixels_sent < total_pixels)
    {
        int chunk = total_pixels - pixels_sent;
        if (chunk > RECT_BUFFER_SIZE)
            chunk = RECT_BUFFER_SIZE;
        send_data_dma((uint8_t*)g_rect_buffer, chunk * 2);
        pixels_sent += chunk;
    }
}

// No-op for compatibility - direct writes don't need explicit update
bool st7789_async_update(void)
{
    return true;
}

// Always ready - no DMA to wait for
bool st7789_async_is_ready(void)
{
    return true;
}

// No-op - no DMA to wait for
void st7789_async_wait(void)
{
}

void st7789_async_get_stats(uint64_t* updates, uint64_t* skipped)
{
    if (updates)
        *updates = g_update_count;
    if (skipped)
        *skipped = 0; // No skipping with direct writes
}
