/* ILI9488 DMA-Accelerated Driver for Waveshare Pico-ResTouch-LCD-3.5"
 * 480x320 RGB565 display with DMA-backed transfers
 * High-performance mode: 62.5 MHz SPI, completion callback delivered from
 * caller context after the transfer and bus release complete.
 *
 * Pin configuration (Waveshare Pico-ResTouch-LCD-3.5):
 * - DC:   GPIO 8
 * - CS:   GPIO 9
 * - SCK:  GPIO 10 (SPI1)
 * - MOSI: GPIO 11 (SPI1)
 * - MISO: GPIO 12 (SPI1)
 * - BL:   GPIO 13
 * - RST:  GPIO 15
 * - TP_CS:  GPIO 16 (touch - active low, keep high)
 * - TP_IRQ: GPIO 17 (touch interrupt)
 * - SD_CS:  GPIO 22 (SD card - active low, keep high)
 */
#pragma once

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Display dimensions (landscape: 480 wide x 320 tall)
#define ILI9488_WIDTH 480
#define ILI9488_HEIGHT 320

    // RGB565 color format (16-bit color)
    typedef uint16_t ili9488_color_t;

    // DMA transfer completion callback type
    typedef void (*ili9488_dma_callback_t)(void);

    // Background color constant - dark blue to match reference panel
    #define ILI9488_BG_R 0
    #define ILI9488_BG_G 0
    #define ILI9488_BG_B 160

    // Create RGB565 color from RGB values with byte swap for SPI
    static inline ili9488_color_t ili9488_rgb565(uint8_t r, uint8_t g, uint8_t b)
    {
        uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        return (color >> 8) | (color << 8); // Byte swap for little-endian SPI
    }

    // Standard background color for the front panel
    static inline ili9488_color_t ili9488_bg_color(void)
    {
        return ili9488_rgb565(ILI9488_BG_R, ILI9488_BG_G, ILI9488_BG_B);
    }

    // Initialize the ILI9488 driver in high-performance mode
    // Returns true on success
    bool ili9488_async_init(void);

    // Register a DMA transfer completion callback
    // Called from normal code after the transfer has completed and the SPI bus
    // has been released.
    void ili9488_async_set_callback(ili9488_dma_callback_t callback);

    // Fill a rectangle directly to display
    void ili9488_async_fill_rect(int x, int y, int w, int h, ili9488_color_t color);

    // Blit a contiguous RGB565 bitmap to the display in one DMA transaction.
    // Pixels must contain w*h entries in row-major order.
    void ili9488_async_blit_rgb565(int x, int y, int w, int h, const ili9488_color_t* pixels);

    // Queue a contiguous RGB565 bitmap blit asynchronously.
    // Returns false if queue is full or parameters are invalid.
    bool ili9488_async_blit_rgb565_async(int x, int y, int w, int h, const ili9488_color_t* pixels);

    // Non-blocking service routine for DMA queue progression/completion.
    // Call periodically from the main/poll loop.
    void ili9488_async_service(void);

    // Returns true if an async blit can be queued now.
    bool ili9488_async_queue_available(void);

    // Draw a row of LEDs efficiently
    // bits: bit pattern for LED states (bit 0 = rightmost LED)
    // num_leds: number of LEDs to draw
    // x_start: starting X position
    // y: Y position
    // led_size: size of each LED (square)
    // spacing: distance between LED left edges
    // on_color: color for ON state
    // off_color: color for OFF state
    void ili9488_async_draw_led_row(uint32_t bits, int num_leds, int x_start, int y,
                                    int led_size, int spacing,
                                    ili9488_color_t on_color, ili9488_color_t off_color);

    // Draw text directly to display (capital letters, numbers, punctuation)
    void ili9488_async_text(const char* str, int x, int y, ili9488_color_t color, ili9488_color_t bg_color);

    // Draw text at 2x scale for larger displays
    void ili9488_async_text_2x(const char* str, int x, int y, ili9488_color_t color, ili9488_color_t bg_color);

    // Clear the entire screen to a color
    void ili9488_async_clear(ili9488_color_t color);

    // Returns true if DMA is idle (no pending transfer)
    bool ili9488_async_is_ready(void);

    // Block until any pending DMA transfer completes
    void ili9488_async_wait(void);

    // Get transfer statistics
    void ili9488_async_get_stats(uint64_t* updates, uint64_t* dma_completions);

#ifdef __cplusplus
}
#endif
