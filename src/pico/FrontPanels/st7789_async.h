/* Direct-Write ST7789 Driver for Pimoroni Pico Display 2.8"
 * Framebuffer-less implementation - writes directly to display
 * Uses partial window updates for efficient LED rendering (~150KB RAM saved)
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

// Display dimensions (landscape)
#define ST7789_ASYNC_WIDTH 320
#define ST7789_ASYNC_HEIGHT 240

    // RGB565 color format (16-bit color)
    typedef uint16_t color_t;

    // Create RGB565 color from RGB values with byte swap for SPI
    static inline color_t rgb565(uint8_t r, uint8_t g, uint8_t b)
    {
        // RGB565 with byte swap for little-endian SPI
        uint16_t color = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
        return (color >> 8) | (color << 8);
    }

// Compatibility alias
#define rgb332 rgb565

    // Initialize the ST7789 driver
    // Returns true on success
    bool st7789_async_init(void);

    // Fill a rectangle directly to display (for LED drawing)
    void st7789_async_fill_rect(int x, int y, int w, int h, color_t color);

    // Draw a row of LEDs efficiently in one DMA transfer
    // bits: bit pattern for LED states (bit 0 = rightmost LED)
    // num_leds: number of LEDs to draw
    // x_start: starting X position
    // y: Y position
    // led_size: size of each LED (square)
    // spacing: distance between LED left edges
    // on_color: color for ON state
    // off_color: color for OFF state
    void st7789_async_draw_led_row(uint32_t bits, int num_leds, int x_start, int y, 
                                   int led_size, int spacing, color_t on_color, color_t off_color);

    // Draw text directly to display (capital letters and numbers only)
    void st7789_async_text(const char* str, int x, int y, color_t color);

    // Clear the entire screen to a color
    void st7789_async_clear(color_t color);

    // No-op for compatibility - direct writes don't need explicit update
    bool st7789_async_update(void);

    // Always returns true - no DMA buffering
    bool st7789_async_is_ready(void);

    // No-op for compatibility - no DMA to wait for
    void st7789_async_wait(void);

    // Get statistics (skipped always 0 with direct writes)
    void st7789_async_get_stats(uint64_t* updates, uint64_t* skipped);

#ifdef __cplusplus
}
#endif
