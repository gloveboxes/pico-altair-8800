/* ILI9488 DMA-Accelerated Driver for Waveshare Pico-ResTouch-LCD-3.5"
 * 480x320 RGB565 display with DMA-backed transfers
 * High-performance mode: 62.5 MHz SPI, callback delivery deferred until the
 * transfer completes and the shared SPI1 bus has been released.
 *
 * Pin configuration (Waveshare Pico-ResTouch-LCD-3.5):
 * - DC:   GPIO 8
 * - CS:   GPIO 9
 * - SCK:  GPIO 10 (SPI1)
 * - MOSI: GPIO 11 (SPI1)
 * - MISO: GPIO 12 (SPI1)
 * - BL:   GPIO 13
 * - RST:  GPIO 15
 *
 * Based on reference implementation from Waveshare and ILI9488 datasheet.
 * The ILI9488 uses a parallel-to-serial shift register on this board,
 * requiring 16-bit data writes (two bytes per pixel in RGB565 mode).
 */

#include "ili9488_async.h"
#include "font_5x8.h"
#include "hardware/dma.h"
#include "hardware/pwm.h"
#include "spi1_bus.h"
#include <string.h>

// ILI9488 Commands
#define ILI9488_NOP        0x00
#define ILI9488_SWRESET    0x01
#define ILI9488_SLPIN      0x10
#define ILI9488_SLPOUT     0x11
#define ILI9488_NORON      0x13
#define ILI9488_INVOFF     0x20
#define ILI9488_INVON      0x21
#define ILI9488_DISPON     0x29
#define ILI9488_CASET      0x2A
#define ILI9488_RASET      0x2B
#define ILI9488_RAMWR      0x2C
#define ILI9488_MADCTL     0x36
#define ILI9488_COLMOD     0x3A
#define ILI9488_FRMCTR1    0xB1
#define ILI9488_DFUNCTR    0xB6
#define ILI9488_PWCTR1     0xC0
#define ILI9488_PWCTR2     0xC1
#define ILI9488_PWCTR3     0xC2
#define ILI9488_VMCTR1     0xC5
#define ILI9488_PGAMCTRL   0xE0
#define ILI9488_NGAMCTRL   0xE1

// Pin definitions for Waveshare Pico-ResTouch-LCD-3.5
#define PIN_DC   8
#define PIN_CS   9
#define PIN_SCK  10
#define PIN_MOSI 11
#define PIN_MISO 12
#define PIN_BL   13
#define PIN_RST  15
#define PIN_TP_CS  16
#define PIN_SD_CS  22

// SPI instance - Waveshare 3.5" uses SPI1
#define SPI_INST spi1

// SPI clock rate - max tested for this display is 60 MHz
#define SPI_BAUDRATE (62500000)

// Pixel buffer for rectangle fills and LED drawing
// Sized for largest single LED row: 480 pixels wide
#define PIXEL_BUFFER_SIZE 1024
static uint16_t g_pixel_buffer[PIXEL_BUFFER_SIZE];

typedef struct
{
    int x;
    int y;
    int w;
    int h;
    const uint16_t* pixels;
    size_t num_bytes;
} ili9488_transfer_t;

// DMA state
static int g_dma_channel = -1;
static volatile bool g_dma_busy = false;
static volatile bool g_dma_queued = false;
static ili9488_dma_callback_t g_dma_callback = NULL;
static volatile uint32_t g_pending_callback_count = 0;
static bool g_bus_held = false; // Track if we hold the SPI1 bus mutex
static bool g_async_lock_active = false;
static ili9488_transfer_t g_queued_transfer;

static void send_command(uint8_t cmd);
static void send_pixels_dma(const uint16_t* pixels, size_t num_bytes);
static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

// Statistics
static uint64_t g_update_count = 0;
static uint64_t g_dma_completion_count = 0;

static inline void run_pending_callbacks(void)
{
    while (g_pending_callback_count != 0)
    {
        ili9488_dma_callback_t callback = g_dma_callback;

        g_pending_callback_count--;
        if (callback)
        {
            callback();
        }
    }
}

static void start_transfer(const ili9488_transfer_t* t)
{
    set_window((uint16_t)t->x, (uint16_t)t->y,
               (uint16_t)(t->x + t->w - 1), (uint16_t)(t->y + t->h - 1));
    send_command(ILI9488_RAMWR);
    send_pixels_dma(t->pixels, t->num_bytes);
}

static void complete_transfer_if_done(void)
{
    if (!g_dma_busy)
    {
        return;
    }

    if (dma_channel_is_busy(g_dma_channel) || spi_is_busy(SPI_INST))
    {
        return;
    }

    gpio_put(PIN_CS, 1);
    g_dma_busy = false;
    g_dma_completion_count++;

    if (g_dma_callback)
    {
        g_pending_callback_count++;
    }
}

// Wait for any pending DMA transfer to complete
static inline void wait_for_dma(void)
{
    while (g_dma_busy || g_dma_queued)
    {
        ili9488_async_service();
        tight_loop_contents();
    }
}

// Acquire SPI1 bus for LCD use (sets baudrate to LCD speed)
static inline void bus_acquire(void)
{
    while (g_async_lock_active)
    {
        ili9488_async_service();
        tight_loop_contents();
    }

    if (!g_bus_held)
    {
        spi1_bus_acquire_lcd();
        g_bus_held = true;
    }
}

// Release SPI1 bus
static inline void bus_release(void)
{
    if (g_bus_held)
    {
        wait_for_dma(); // Ensure DMA done before releasing
        gpio_put(PIN_CS, 1); // Ensure CS deasserted
        spi1_bus_release();
        g_bus_held = false;
        run_pending_callbacks();
    }
}

// Send command byte to display
static void send_command(uint8_t cmd)
{
    wait_for_dma();
    gpio_put(PIN_DC, 0); // Command mode
    gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_INST, &cmd, 1);
    gpio_put(PIN_CS, 1);
}

// Send data bytes to display (blocking, for init sequences)
static void send_data(const uint8_t* data, size_t len)
{
    wait_for_dma();
    gpio_put(PIN_DC, 1); // Data mode
    gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI_INST, data, len);
    gpio_put(PIN_CS, 1);
}

// Send a single register parameter byte.
// On this Waveshare 3.5" path, control data is transferred as 16-bit words.
static void send_data_byte(uint8_t val)
{
    uint8_t buf[2] = {0x00, val};
    send_data(buf, 2);
}

// Send 16-bit data value (MSB first, as ILI9488 expects)
static void send_data_16(uint16_t val)
{
    uint8_t buf[2] = {val >> 8, val & 0xFF};
    send_data(buf, 2);
}

// Start DMA pixel transfer (non-blocking, completion via wait_for_dma)
static void send_pixels_dma(const uint16_t* pixels, size_t num_bytes)
{
    wait_for_dma();

    gpio_put(PIN_DC, 1); // Data mode
    gpio_put(PIN_CS, 0);

    g_dma_busy = true;
    dma_channel_set_read_addr(g_dma_channel, pixels, false);
    dma_channel_set_trans_count(g_dma_channel, num_bytes, true);
}

// Set display window for pixel writing
static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1)
{
    wait_for_dma();

    // Column address set
    send_command(ILI9488_CASET);
    send_data_byte(x0 >> 8);
    send_data_byte(x0 & 0xFF);
    send_data_byte(x1 >> 8);
    send_data_byte(x1 & 0xFF);

    // Row address set
    send_command(ILI9488_RASET);
    send_data_byte(y0 >> 8);
    send_data_byte(y0 & 0xFF);
    send_data_byte(y1 >> 8);
    send_data_byte(y1 & 0xFF);
}

bool ili9488_async_init(void)
{
    // Shared SPI1 bus: peripheral, data pins, and all CS pins are
    // initialized by spi1_bus_init().  We only set up display-specific
    // control pins here (DC, backlight, reset, DMA).
    spi1_bus_init();

    // Display-specific control pins (DC, CS accent managed locally)
    gpio_init(PIN_DC);
    gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_init(PIN_CS);
    gpio_set_dir(PIN_CS, GPIO_OUT);
    gpio_put(PIN_CS, 1);

    // Set up backlight with PWM at full brightness
    gpio_set_function(PIN_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_BL);
    pwm_set_wrap(slice, 65535);
    pwm_set_gpio_level(PIN_BL, 65535); // Full brightness (high-power mode)
    pwm_set_enabled(slice, true);

    // Claim and configure DMA channel (completion is polled in wait_for_dma)
    g_dma_channel = dma_claim_unused_channel(true);
    dma_channel_config dma_cfg = dma_channel_get_default_config(g_dma_channel);
    channel_config_set_transfer_data_size(&dma_cfg, DMA_SIZE_8);
    channel_config_set_dreq(&dma_cfg, spi_get_dreq(SPI_INST, true));
    channel_config_set_read_increment(&dma_cfg, true);
    channel_config_set_write_increment(&dma_cfg, false);
    dma_channel_configure(g_dma_channel, &dma_cfg,
                          &spi_get_hw(SPI_INST)->dr, NULL, 0, false);

    // Hardware reset
    gpio_init(PIN_RST);
    gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_put(PIN_RST, 1);
    sleep_ms(10);
    gpio_put(PIN_RST, 0); // Reset pulse (active low)
    sleep_ms(10);
    gpio_put(PIN_RST, 1); // Release reset
    sleep_ms(120);         // Wait for controller startup

    // === ILI9488 initialization sequence (matches Waveshare reference) ===
    bus_acquire();

    // Display Inversion ON (required for correct colors on this panel)
    send_command(ILI9488_INVON);

    // Power Control 3 - Normal mode, increase display quality
    send_command(ILI9488_PWCTR3);
    send_data_byte(0x33);

    // VCOM Control
    send_command(ILI9488_VMCTR1);
    send_data_byte(0x00);
    send_data_byte(0x1E); // VCM_REG[7:0] <= 0x80
    send_data_byte(0x80);

    // Frame Rate Control - 70Hz for smooth animation
    send_command(ILI9488_FRMCTR1);
    send_data_byte(0xB0); // 70Hz

    // Memory Access Control - Landscape orientation
    // MV=1, MX=0, MY=0, BGR=1 -> 0x28
    send_command(ILI9488_MADCTL);
    send_data_byte(0x28);

    // Positive Gamma Control
    send_command(ILI9488_PGAMCTRL);
    {
        uint8_t pgamma[] = {0x00, 0x13, 0x18, 0x04, 0x0F, 0x06,
                            0x3A, 0x56, 0x4D, 0x03, 0x0A, 0x06,
                            0x30, 0x3E, 0x0F};
        send_data(pgamma, sizeof(pgamma));
    }

    // Negative Gamma Control
    send_command(ILI9488_NGAMCTRL);
    {
        uint8_t ngamma[] = {0x00, 0x13, 0x18, 0x01, 0x11, 0x06,
                            0x38, 0x34, 0x4D, 0x06, 0x0D, 0x0B,
                            0x31, 0x37, 0x0F};
        send_data(ngamma, sizeof(ngamma));
    }

    // Interface Pixel Format - 16-bit RGB565
    send_command(ILI9488_COLMOD);
    send_data_byte(0x55);

    // Display Function Control - required for correct scan direction on 3.5" panel
    // SS=0, SM=0 -> source driver scan from S1 to S960
    send_command(ILI9488_DFUNCTR);
    send_data_byte(0x00);
    send_data_byte(0x62); // Landscape flipped: REV=0, GS=1, SS=1, SM=0

    // Sleep Out
    send_command(ILI9488_SLPOUT);
    sleep_ms(120);

    // Display ON
    send_command(ILI9488_DISPON);
    sleep_ms(10);

    bus_release();

    // Clear screen to black
    ili9488_async_clear(0);

    return true;
}

void ili9488_async_set_callback(ili9488_dma_callback_t callback)
{
    g_dma_callback = callback;
}

void ili9488_async_fill_rect(int x, int y, int w, int h, ili9488_color_t color)
{
    // Clamp to screen bounds
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > ILI9488_WIDTH)  w = ILI9488_WIDTH - x;
    if (y + h > ILI9488_HEIGHT) h = ILI9488_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    bus_acquire();

    // Set window
    set_window(x, y, x + w - 1, y + h - 1);
    send_command(ILI9488_RAMWR);

    int total_pixels = w * h;

    // Fill buffer with color
    int buf_pixels = (total_pixels < PIXEL_BUFFER_SIZE) ? total_pixels : PIXEL_BUFFER_SIZE;
    for (int i = 0; i < buf_pixels; i++)
    {
        g_pixel_buffer[i] = color;
    }

    // Send data in chunks via DMA
    int pixels_sent = 0;
    while (pixels_sent < total_pixels)
    {
        int chunk = total_pixels - pixels_sent;
        if (chunk > buf_pixels) chunk = buf_pixels;
        send_pixels_dma(g_pixel_buffer, chunk * 2);
        wait_for_dma(); // Must wait before reusing buffer
        pixels_sent += chunk;
    }

    bus_release();
    g_update_count++;
}

void ili9488_async_blit_rgb565(int x, int y, int w, int h, const ili9488_color_t* pixels)
{
    if (!pixels)
        return;

    // Require fully in-bounds region for a single contiguous transfer.
    if (x < 0 || y < 0 || w <= 0 || h <= 0)
        return;
    if (x + w > ILI9488_WIDTH || y + h > ILI9488_HEIGHT)
        return;

    bus_acquire();
    set_window((uint16_t)x, (uint16_t)y, (uint16_t)(x + w - 1), (uint16_t)(y + h - 1));
    send_command(ILI9488_RAMWR);

    size_t num_bytes = (size_t)w * (size_t)h * sizeof(ili9488_color_t);
    send_pixels_dma((const uint16_t*)pixels, num_bytes);
    wait_for_dma();

    bus_release();
    g_update_count++;
}

bool ili9488_async_blit_rgb565_async(int x, int y, int w, int h, const ili9488_color_t* pixels)
{
    if (!pixels)
        return false;

    if (x < 0 || y < 0 || w <= 0 || h <= 0)
        return false;
    if (x + w > ILI9488_WIDTH || y + h > ILI9488_HEIGHT)
        return false;

    ili9488_async_service();

    if (g_dma_queued)
        return false;

    ili9488_transfer_t t = {
        .x = x,
        .y = y,
        .w = w,
        .h = h,
        .pixels = (const uint16_t*)pixels,
        .num_bytes = (size_t)w * (size_t)h * sizeof(ili9488_color_t),
    };

    if (!g_dma_busy)
    {
        bus_acquire();
        g_async_lock_active = true;
        start_transfer(&t);
    }
    else
    {
        g_queued_transfer = t;
        g_dma_queued = true;
    }

    g_update_count++;
    return true;
}

void ili9488_async_service(void)
{
    complete_transfer_if_done();

    if (!g_dma_busy && g_dma_queued)
    {
        g_dma_queued = false;
        start_transfer(&g_queued_transfer);
    }

    if (!g_dma_busy && !g_dma_queued && g_async_lock_active)
    {
        if (g_bus_held)
        {
            gpio_put(PIN_CS, 1);
            spi1_bus_release();
            g_bus_held = false;
        }
        g_async_lock_active = false;
        run_pending_callbacks();
    }
}

bool ili9488_async_queue_available(void)
{
    ili9488_async_service();
    return !g_dma_queued;
}

void ili9488_async_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                                int led_size, int spacing,
                                ili9488_color_t on_color, ili9488_color_t off_color)
{
    // Calculate total row width
    int total_width = (num_leds - 1) * spacing + led_size;

    // Bounds check
    if (x_start < 0 || y < 0 ||
        x_start + total_width > ILI9488_WIDTH ||
        y + led_size > ILI9488_HEIGHT)
        return;
    if (total_width > PIXEL_BUFFER_SIZE)
        return;

    bus_acquire();

    // Set window for entire row
    set_window(x_start, y, x_start + total_width - 1, y + led_size - 1);
    send_command(ILI9488_RAMWR);

    // Background color (between LEDs)
    ili9488_color_t bg_color = ili9488_rgb565(0, 0, 0);

    // Build one scanline of the LED row
    for (int px = 0; px < total_width; px++)
    {
        // Determine which LED (if any) this pixel is within
        int led_idx = px / spacing;
        int within_led = px - (led_idx * spacing);

        if (led_idx < num_leds && within_led < led_size)
        {
            // Inside an LED
            int bit_idx = num_leds - 1 - led_idx; // MSB first
            bool on = (bits >> bit_idx) & 1;
            g_pixel_buffer[px] = on ? on_color : off_color;
        }
        else
        {
            g_pixel_buffer[px] = bg_color;
        }
    }

    // Send the same scanline pattern for each row of the LED
    for (int row = 0; row < led_size; row++)
    {
        send_pixels_dma((uint16_t*)g_pixel_buffer, total_width * 2);
        wait_for_dma();
    }

    bus_release();
    g_update_count++;
}

void ili9488_async_text(const char* str, int x, int y, ili9488_color_t color, ili9488_color_t bg_color)
{
    bus_acquire();

    while (*str)
    {
        char c = *str++;
        int glyph_idx = -1;

        if (c >= 'A' && c <= 'Z')
            glyph_idx = c - 'A';
        else if (c >= 'a' && c <= 'z')
            glyph_idx = c - 'a'; // Map lowercase to uppercase glyphs
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
        else if (c == '/')
            glyph_idx = 41;
        else if (c == ' ')
        {
            x += 6;
            continue;
        }

        if (glyph_idx >= 0 && glyph_idx < FONT_GLYPH_COUNT)
        {
            // Set window for this character (6x8)
            set_window(x, y, x + 5, y + 7);
            send_command(ILI9488_RAMWR);

            // Build character pixels
            uint16_t char_buf[48]; // 6 cols x 8 rows
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
                        char_buf[idx++] = bg_color; // 1px spacing
                    }
                }
            }
            send_data((uint8_t*)char_buf, sizeof(char_buf));
            x += 6;
        }
    }
    bus_release();
}

void ili9488_async_text_2x(const char* str, int x, int y, ili9488_color_t color, ili9488_color_t bg_color)
{
    bus_acquire();

    while (*str)
    {
        char c = *str++;
        int glyph_idx = -1;

        if (c >= 'A' && c <= 'Z')
            glyph_idx = c - 'A';
        else if (c >= 'a' && c <= 'z')
            glyph_idx = c - 'a';
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
        else if (c == '/')
            glyph_idx = 41;
        else if (c == ' ')
        {
            x += 12;
            continue;
        }

        if (glyph_idx >= 0 && glyph_idx < FONT_GLYPH_COUNT)
        {
            // Set window for 2x character (12x16)
            set_window(x, y, x + 11, y + 15);
            send_command(ILI9488_RAMWR);

            // Build 2x scaled character pixels (12 cols x 16 rows = 192 pixels)
            uint16_t char_buf[192];
            int idx = 0;
            for (int row = 0; row < 16; row++)
            {
                int src_row = row / 2;
                for (int col = 0; col < 12; col++)
                {
                    int src_col = col / 2;
                    if (src_col < 5)
                    {
                        uint8_t column_data = font_5x8[glyph_idx][src_col];
                        char_buf[idx++] = (column_data & (1 << src_row)) ? color : bg_color;
                    }
                    else
                    {
                        char_buf[idx++] = bg_color;
                    }
                }
            }
            send_data((uint8_t*)char_buf, sizeof(char_buf));
            x += 12;
        }
    }
    bus_release();
}

void ili9488_async_clear(ili9488_color_t color)
{
    bus_acquire();
    set_window(0, 0, ILI9488_WIDTH - 1, ILI9488_HEIGHT - 1);
    send_command(ILI9488_RAMWR);

    // Fill buffer
    for (int i = 0; i < PIXEL_BUFFER_SIZE; i++)
    {
        g_pixel_buffer[i] = color;
    }

    // Send in chunks - total pixels = 480*320 = 153600
    int total_pixels = ILI9488_WIDTH * ILI9488_HEIGHT;
    int pixels_sent = 0;
    while (pixels_sent < total_pixels)
    {
        int chunk = total_pixels - pixels_sent;
        if (chunk > PIXEL_BUFFER_SIZE) chunk = PIXEL_BUFFER_SIZE;
        send_pixels_dma(g_pixel_buffer, chunk * 2);
        wait_for_dma();
        pixels_sent += chunk;
    }
    bus_release();
}

bool ili9488_async_is_ready(void)
{
    return !g_dma_busy && !g_dma_queued;
}

void ili9488_async_wait(void)
{
    wait_for_dma();
    ili9488_async_service();
    run_pending_callbacks();
}

void ili9488_async_get_stats(uint64_t* updates, uint64_t* dma_completions)
{
    if (updates) *updates = g_update_count;
    if (dma_completions) *dma_completions = g_dma_completion_count;
}
