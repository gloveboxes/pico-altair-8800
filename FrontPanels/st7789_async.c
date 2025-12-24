/* Custom Async ST7789 Driver for Pico Display 2.8"
 * Simplified non-blocking implementation for LED display
 *
 * Pin configuration (Pimoroni Pico Display 2.8"):
 * - DC: GPIO 16
 * - CS: GPIO 17
 * - SCK: GPIO 18
 * - MOSI: GPIO 19
 * - BL: GPIO 20
 */

#include "st7789_async.h"
#include "hardware/pwm.h"
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

// Pin definitions for Pico Display 2.8"
#define PIN_DC 16
#define PIN_CS 17
#define PIN_SCK 18
#define PIN_MOSI 19
#define PIN_BL 20
#define PIN_LED_R 26
#define PIN_LED_G 27
#define PIN_LED_B 28

// SPI instance
#define SPI_INST spi0

// Framebuffer (320x240 x 2 bytes RGB565)
static uint16_t g_framebuffer[ST7789_ASYNC_WIDTH * ST7789_ASYNC_HEIGHT];

// DMA channel
static int g_dma_channel = -1;
static volatile bool g_dma_busy = false;

// Statistics
static uint64_t g_update_count = 0;
static uint64_t g_skip_count = 0;

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

// Send data to display
static void send_data(const uint8_t* data, size_t len)
{
    gpio_put(PIN_DC, 1); // Data mode
    gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_INST, data, len);
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

// DMA completion IRQ handler
static void dma_irq_handler(void)
{
    if (dma_channel_get_irq0_status(g_dma_channel))
    {
        dma_channel_acknowledge_irq0(g_dma_channel);
        g_dma_busy = false;
        gpio_put(PIN_CS, 1); // Deassert CS when done
    }
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

    // Claim DMA channel
    g_dma_channel = dma_claim_unused_channel(true);

    // Configure DMA for 8-bit transfers (RGB565 sent as bytes)
    dma_channel_config config = dma_channel_get_default_config(g_dma_channel);
    channel_config_set_transfer_data_size(&config, DMA_SIZE_8);
    channel_config_set_dreq(&config, spi_get_dreq(SPI_INST, true));
    dma_channel_configure(g_dma_channel, &config, &spi_get_hw(SPI_INST)->dr, NULL, 0, false);

    // Set up DMA IRQ
    dma_channel_set_irq0_enabled(g_dma_channel, true);
    irq_set_exclusive_handler(DMA_IRQ_0, dma_irq_handler);
    irq_set_enabled(DMA_IRQ_0, true);

    // Reset display
    send_command(ST7789_SWRESET);
    sleep_ms(150);

    // Initialize display
    send_command(ST7789_SLPOUT);
    sleep_ms(10);

    // Color mode: RGB565 (16-bit color) - matches Pimoroni
    send_command(ST7789_COLMOD);
    uint8_t colmod = 0x05; // 16-bit/pixel
    send_data(&colmod, 1);

    // Memory access control for 320x240 (rotated 180 degrees)
    send_command(ST7789_MADCTL);
    uint8_t madctl = 0xE4; // MY + MX + MV + RGB for 180-degree rotation
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

    // Clear framebuffer
    st7789_async_clear(0);

    return true;
}

static void st7789_async_set_pixel(int x, int y, color_t color)
{
    if (x >= 0 && x < ST7789_ASYNC_WIDTH && y >= 0 && y < ST7789_ASYNC_HEIGHT)
    {
        g_framebuffer[y * ST7789_ASYNC_WIDTH + x] = color;
    }
}

// Simple text rendering
void st7789_async_text(const char* str, int x, int y, color_t color)
{
    // Calculate string length
    int len = 0;
    while (str[len])
        len++;

    // Draw string in reverse order
    for (int i = len - 1; i >= 0; i--)
    {
        char c = str[i];
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
            // Draw 5x8 character (flipped horizontally)
            for (int col = 0; col < 5; col++)
            {
                uint8_t column_data = font_5x8[glyph_idx][4 - col]; // Flip horizontally
                for (int row = 0; row < 8; row++)
                {
                    if (column_data & (1 << row))
                    {
                        st7789_async_set_pixel(x + col, y + row, color);
                    }
                }
            }
            x += 6; // 5 pixels + 1 space
        }
    }
}

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

    // Direct framebuffer access (much faster than set_pixel per pixel)
    for (int dy = 0; dy < h; dy++)
    {
        uint16_t* row = &g_framebuffer[(y + dy) * ST7789_ASYNC_WIDTH + x];
        for (int dx = 0; dx < w; dx++)
        {
            row[dx] = color;
        }
    }
}

void st7789_async_clear(color_t color)
{
    for (int i = 0; i < ST7789_ASYNC_WIDTH * ST7789_ASYNC_HEIGHT; i++)
    {
        g_framebuffer[i] = color;
    }
}

bool st7789_async_update(void)
{
    // Check if DMA is still busy
    if (g_dma_busy)
    {
        g_skip_count++;
        return false;
    }

    // Set window to full screen
    set_window(0, 0, ST7789_ASYNC_WIDTH - 1, ST7789_ASYNC_HEIGHT - 1);

    // Start RAM write
    send_command(ST7789_RAMWR);

    // Set up for data mode and assert CS
    gpio_put(PIN_DC, 1);
    gpio_put(PIN_CS, 0);

    // Start DMA transfer (non-blocking!) - 2x size for 16-bit pixels as bytes
    g_dma_busy = true;
    dma_channel_set_read_addr(g_dma_channel, g_framebuffer, false);
    dma_channel_set_trans_count(g_dma_channel, sizeof(g_framebuffer), true);

    g_update_count++;
    return true;
}

bool st7789_async_is_ready(void)
{
    return !g_dma_busy;
}

void st7789_async_wait(void)
{
    while (g_dma_busy)
    {
        tight_loop_contents();
    }
}

void st7789_async_get_stats(uint64_t* updates, uint64_t* skipped)
{
    if (updates)
        *updates = g_update_count;
    if (skipped)
        *skipped = g_skip_count;
}
