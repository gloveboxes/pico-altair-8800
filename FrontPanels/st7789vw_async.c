/* Custom Direct-Write ST7789VW Driver for Waveshare 2" LCD
 * Framebuffer-less implementation with DMA acceleration
 * Saves ~150KB RAM by using partial window updates with DMA transfers
 *
 * Pin configuration for Waveshare 2" LCD:
 * - DC: GPIO 8
 * - CS: GPIO 9
 * - SCK: GPIO 10 (SPI1)
 * - MOSI: GPIO 11 (SPI1)
 * - RST: GPIO 12
 * - BL: GPIO 13
 */

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include "st7789_async.h"
#include <string.h>

// ST7789VW Commands
#define ST7789_SWRESET 0x01
#define ST7789_SLPOUT 0x11
#define ST7789_NORON 0x13
#define ST7789_COLMOD 0x3A
#define ST7789_MADCTL 0x36
#define ST7789_CASET 0x2A
#define ST7789_RASET 0x2B
#define ST7789_RAMWR 0x2C
#define ST7789_DISPON 0x29
#define ST7789_INVON 0x21

// Pin definitions for Waveshare 2" LCD
#define PIN_DC 8
#define PIN_CS 9
#define PIN_SCK 10
#define PIN_MOSI 11
#define PIN_RST 12
#define PIN_BL 13

// SPI instance - Waveshare uses GPIO 10/11 which are SPI1
#define SPI_INST spi1

// Small buffer for rectangle fills (max LED size 15x15 = 225 pixels = 450 bytes)
#define RECT_BUFFER_SIZE 512
static uint16_t g_rect_buffer[RECT_BUFFER_SIZE];

// DMA channel for pixel data transfers
static int g_dma_channel = -1;
static volatile bool g_dma_busy = false;

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

// Send command to display
static void send_command(uint8_t cmd)
{
    gpio_put(PIN_DC, 0); // Command mode
    gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_INST, &cmd, 1);
    gpio_put(PIN_CS, 1);
}

// Send data to display (blocking, for small transfers like commands)
static void send_data(const uint8_t* data, size_t len)
{
    gpio_put(PIN_DC, 1); // Data mode
    gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_INST, data, len);
    gpio_put(PIN_CS, 1);
}

// Wait for any pending DMA transfer to complete
static inline void wait_for_dma(void)
{
    while (g_dma_busy)
    {
        if (!dma_channel_is_busy(g_dma_channel))
        {
            // DMA done transferring to SPI FIFO, but wait for SPI to finish transmitting
            while (spi_is_busy(SPI_INST))
            {
                tight_loop_contents();
            }
            g_dma_busy = false;
            gpio_put(PIN_CS, 1); // Deassert CS when done
        }
    }
}

// Send pixel data using DMA (non-blocking start, but we wait before next operation)
static void send_pixels_dma(const uint16_t* pixels, size_t num_pixels)
{
    // Wait for any previous DMA to complete
    wait_for_dma();
    
    // Set up for data mode and assert CS
    gpio_put(PIN_DC, 1);
    gpio_put(PIN_CS, 0);
    
    // Start DMA transfer
    g_dma_busy = true;
    dma_channel_set_read_addr(g_dma_channel, pixels, false);
    dma_channel_set_trans_count(g_dma_channel, num_pixels * 2, true); // * 2 for bytes
}

// Set display window for pixel writing
static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    // Wait for any pending DMA before changing window
    wait_for_dma();
    
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
    // Initialize SPI1 at 75 MHz (matching Pimoroni driver)
    spi_init(SPI_INST, 75000000);

    // Set up SPI pins
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    // Set up control pins
    gpio_init(PIN_DC);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    // Set up backlight (PWM)
    gpio_set_function(PIN_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_BL);
    pwm_set_wrap(slice, 65535);
    pwm_set_gpio_level(PIN_BL, 65535); // Full brightness
    pwm_set_enabled(slice, true);

    // Claim and configure DMA channel for pixel data transfers
    g_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config config = dma_channel_get_default_config(g_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
    channel_config_set_dreq(&config, spi_get_dreq(SPI_INST, true));
    dma_channel_configure(g_dma_channel, &config, &spi_get_hw(SPI_INST)->dr, NULL, 0, false);

    // Hardware reset
    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_put(PIN_RST, 1);
    sleep_ms(10);
    gpio_put(PIN_RST, 0); // Reset pulse (active low)
    sleep_ms(10);
    gpio_put(PIN_RST, 1); // Release reset
    sleep_ms(120);        // Wait for display to initialize

    // Software reset
    send_command(ST7789_SWRESET);
    sleep_ms(150);

    // Sleep out
    send_command(ST7789_SLPOUT);
    sleep_ms(120);

    // ST7789VW-specific initialization sequence

    // Porch Setting - REMOVED (using default)
    // send_command(0xB2);
    // uint8_t porch[] = {0x0C, 0x0C, 0x00, 0x33, 0x33};
    // send_data(porch, sizeof(porch));

    // Gate Control
    send_command(0xB7);
    uint8_t gctrl = 0x35;
    send_data(&gctrl, 1);

    // VCOM Setting
    send_command(0xBB);
    uint8_t vcoms = 0x19;
    send_data(&vcoms, 1);

    // LCM Control
    send_command(0xC0);
    uint8_t lcm = 0x2C;
    send_data(&lcm, 1);

    // VDV and VRH Command Enable
    send_command(0xC2);
    uint8_t vdv_vrh_en = 0x01;
    send_data(&vdv_vrh_en, 1);

    // VRH Set
    send_command(0xC3);
    uint8_t vrhs = 0x12;
    send_data(&vrhs, 1);

    // VDV Set
    send_command(0xC4);
    uint8_t vdv = 0x20;
    send_data(&vdv, 1);

    // Frame Rate Control - REMOVED (using default)
    // send_command(0xC6);
    // uint8_t frctrl = 0x0F; // 60Hz
    // send_data(&frctrl, 1);

    // Power Control 1
    send_command(0xD0);
    uint8_t pwctrl[] = {0xA4, 0xA1};
    send_data(pwctrl, sizeof(pwctrl));

    // Positive Gamma
    send_command(0xE0);
    uint8_t pgamma[] = {0xD0, 0x04, 0x0D, 0x11, 0x13, 0x2B, 0x3F, 0x54, 0x4C, 0x18, 0x0D, 0x0B, 0x1F, 0x23};
    send_data(pgamma, sizeof(pgamma));

    // Negative Gamma
    send_command(0xE1);
    uint8_t ngamma[] = {0xD0, 0x04, 0x0C, 0x11, 0x13, 0x2C, 0x3F, 0x44, 0x51, 0x2F, 0x1F, 0x1F, 0x20, 0x23};
    send_data(ngamma, sizeof(ngamma));

    // Color mode: RGB565 (16-bit color)
    send_command(ST7789_COLMOD);
    uint8_t colmod = 0x05; // 16-bit/pixel
    send_data(&colmod, 1);

    // Memory access control for 320x240 landscape
    send_command(ST7789_MADCTL);
    uint8_t madctl = 0xA0; // MV + MY (Landscape, X-flipped?)
    send_data(&madctl, 1);

    // Inversion on (required for correct colors on ST7789VW)
    send_command(ST7789_INVON);

    // Normal display mode
    send_command(ST7789_NORON);
    sleep_ms(10);

    // Display on
    send_command(ST7789_DISPON);
    sleep_ms(10);

    // Clear framebuffer
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

// Draw a row of LEDs efficiently with one window setup and DMA transfers per scanline
// This draws the entire row (LEDs + gaps) in a single window operation
void st7789_async_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                               int led_size, int spacing, color_t on_color, color_t off_color)
{
    // Calculate total width of the row
    int total_width = (num_leds - 1) * spacing + led_size;
    
    // Clamp to screen bounds
    if (x_start < 0 || y < 0 || x_start + total_width > ST7789_ASYNC_WIDTH || y + led_size > ST7789_ASYNC_HEIGHT)
        return;
    if (total_width > RECT_BUFFER_SIZE)
        return;  // Row too wide for buffer
    
    // Set window for entire row - one window setup for all scanlines
    set_window(x_start, y, x_start + total_width - 1, y + led_size - 1);
    send_command(ST7789_RAMWR);
    
    // Background color (between LEDs)
    color_t bg_color = rgb565(0, 0, 0);
    
    // Build one scanline pattern (same for all rows since LEDs are square)
    int buf_idx = 0;
    for (int led = num_leds - 1; led >= 0; led--)  // MSB on left
    {
        // Determine LED color based on bit pattern
        color_t led_color = (bits >> led) & 1 ? on_color : off_color;
        
        // Draw LED pixels
        for (int px = 0; px < led_size; px++)
        {
            g_rect_buffer[buf_idx++] = led_color;
        }
        
        // Draw gap pixels (except after last LED)
        if (led > 0)
        {
            int gap = spacing - led_size;
            for (int px = 0; px < gap; px++)
            {
                g_rect_buffer[buf_idx++] = bg_color;
            }
        }
    }
    
    int scanline_pixels = buf_idx;
    
    // Send the same scanline pattern for each row of the LED height
    // Set up for data mode and assert CS once
    gpio_put(PIN_DC, 1);
    gpio_put(PIN_CS, 0);
    
    for (int row = 0; row < led_size; row++)
    {
        // Wait for any previous DMA to complete (but don't deassert CS)
        while (dma_channel_is_busy(g_dma_channel))
        {
            tight_loop_contents();
        }
        
        // Start DMA transfer for this scanline
        dma_channel_set_read_addr(g_dma_channel, g_rect_buffer, false);
        dma_channel_set_trans_count(g_dma_channel, scanline_pixels * 2, true);
    }
    
    // Wait for final DMA and SPI to complete
    while (dma_channel_is_busy(g_dma_channel))
    {
        tight_loop_contents();
    }
    while (spi_is_busy(SPI_INST))
    {
        tight_loop_contents();
    }
    
    gpio_put(PIN_CS, 1);
    g_dma_busy = false;
    g_update_count++;
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

    // Send data using DMA in chunks
    int pixels_sent = 0;
    while (pixels_sent < total_pixels)
    {
        int chunk = total_pixels - pixels_sent;
        if (chunk > buf_pixels)
            chunk = buf_pixels;
        
        // Use DMA for pixel data transfer
        send_pixels_dma(g_rect_buffer, chunk);
        pixels_sent += chunk;
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
        send_pixels_dma(g_rect_buffer, chunk);
        pixels_sent += chunk;
    }
    
    // Ensure final DMA completes
    wait_for_dma();
}

// No-op for compatibility - direct writes don't need explicit update
bool st7789_async_update(void)
{
    return true;
}

// Check if DMA transfer is complete
bool st7789_async_is_ready(void)
{
    if (g_dma_busy && !dma_channel_is_busy(g_dma_channel))
    {
        g_dma_busy = false;
        gpio_put(PIN_CS, 1);
    }
    return !g_dma_busy;
}

// Wait for any pending DMA transfer to complete
void st7789_async_wait(void)
{
    wait_for_dma();
}

void st7789_async_get_stats(uint64_t* updates, uint64_t* skipped)
{
    if (updates)
        *updates = g_update_count;
    if (skipped)
        *skipped = 0; // No skipping with direct writes
}
