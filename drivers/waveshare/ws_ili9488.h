/* Waveshare Pico-ResTouch-LCD-3.5 — ILI9488 DMA display driver.
 *
 * 480×320 RGB565, DMA-backed SPI transfers at 37.5 MHz (requested 62.5 MHz).
 * All shared SPI1 bus ownership goes through ws_spi1_bus.
 *
 * Display-specific pins managed here:
 *   DC   — GPIO 8   (data/command select)
 *   CS   — GPIO 9   (directly toggled after bus is acquired)
 *   BL   — GPIO 13  (backlight, PWM)
 *   RST  — GPIO 15  (hardware reset, active low)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Display geometry ---- */
#define WS_LCD_WIDTH  480
#define WS_LCD_HEIGHT 320

/* ---- Colour helpers ---- */
typedef uint16_t ws_color_t;

typedef void (*ws_dma_callback_t)(void);

static inline ws_color_t ws_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    uint16_t c = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (c >> 8) | (c << 8);   /* byte-swap for little-endian SPI */
}

/* ---- Lifecycle ---- */

/// Initialise the ILI9488 display (calls ws_spi1_init internally).
bool ws_ili9488_init(void);

/// Register a DMA completion callback (called from poll context).
void ws_ili9488_set_callback(ws_dma_callback_t cb);

/* ---- Drawing primitives ---- */

void ws_ili9488_clear(ws_color_t color);
void ws_ili9488_fill_rect(int x, int y, int w, int h, ws_color_t color);
void ws_ili9488_blit(int x, int y, int w, int h, const ws_color_t* pixels);

/// Queue an async blit (returns false if the queue is full).
bool ws_ili9488_blit_async(int x, int y, int w, int h, const ws_color_t* pixels);

/// Non-blocking service routine — call from the poll loop.
void ws_ili9488_service(void);

/// Returns true when no DMA transfer is in flight or queued.
bool ws_ili9488_is_ready(void);

/// Block until all pending DMA completes.
void ws_ili9488_wait(void);

/* ---- Text ---- */

void ws_ili9488_text(const char* s, int x, int y, ws_color_t fg, ws_color_t bg);
void ws_ili9488_text_2x(const char* s, int x, int y, ws_color_t fg, ws_color_t bg);

/* ---- Hardware scroll ---- */

void ws_ili9488_set_scroll_area(uint16_t tfa, uint16_t vsa, uint16_t bfa);
void ws_ili9488_set_scroll_start(uint16_t vsp);

/* ---- Stats ---- */
void ws_ili9488_get_stats(uint64_t* updates, uint64_t* dma_completions);

#ifdef __cplusplus
}
#endif
