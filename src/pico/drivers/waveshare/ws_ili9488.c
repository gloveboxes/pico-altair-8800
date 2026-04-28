/* Waveshare Pico-ResTouch-LCD-3.5 — ILI9488 DMA display driver.
 *
 * All shared-bus access goes through ws_spi1_bus.  This file only
 * manages display-specific pins (DC, CS, backlight, reset) and DMA.
 */

#include "ws_ili9488.h"
#include "ws_spi1_bus.h"
#include "FrontPanels/font_5x8.h"
#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/pwm.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"
#include <string.h>

/* ---- ILI9488 command set (subset) ---- */
#define CMD_INVON    0x21
#define CMD_DISPON   0x29
#define CMD_CASET    0x2A
#define CMD_RASET    0x2B
#define CMD_RAMWR    0x2C
#define CMD_MADCTL   0x36
#define CMD_COLMOD   0x3A
#define CMD_FRMCTR1  0xB1
#define CMD_DFUNCTR  0xB6
#define CMD_PWCTR3   0xC2
#define CMD_VMCTR1   0xC5
#define CMD_VSCRDEF  0x33
#define CMD_VSCRSADD 0x37
#define CMD_PGAMCTRL 0xE0
#define CMD_NGAMCTRL 0xE1
#define CMD_SLPOUT   0x11

/* ---- Display-specific pins ---- */
#define PIN_DC  8
#define PIN_CS  WS_PIN_LCD_CS   /* GPIO 9 — managed by spi1_bus for init */
#define PIN_BL  13
#define PIN_RST 15

#define SPI spi1

/* ---- Pixel buffer for fill/LED operations ---- */
#define PBUF_SIZE 1024
static uint16_t g_pbuf[PBUF_SIZE];

/* ---- Async transfer queue (depth 1) ---- */
typedef struct { int x, y, w, h; const uint16_t* px; size_t bytes; } xfer_t;

static int             g_dma_ch = -1;
static volatile bool   g_dma_busy   = false;
static volatile bool   g_dma_queued = false;
static xfer_t          g_queued;
static ws_dma_callback_t g_cb        = NULL;
static volatile uint32_t g_pending_cb = 0;
static bool            g_bus_held   = false;
static bool            g_async_lock = false;
static uint64_t        g_updates    = 0;
static uint64_t        g_completions = 0;

/* ---- Internal helpers ---- */

static inline void run_cbs(void) {
    while (g_pending_cb) { g_pending_cb--; if (g_cb) g_cb(); }
}

static void complete_if_done(void) {
    if (!g_dma_busy) return;
    if (dma_channel_is_busy(g_dma_ch) || spi_is_busy(SPI)) return;
    gpio_put(PIN_CS, 1);
    g_dma_busy = false;
    g_completions++;
    if (g_cb) g_pending_cb++;
}

static inline void wait_dma(void) {
    while (g_dma_busy || g_dma_queued) { ws_ili9488_service(); tight_loop_contents(); }
}

static inline void bus_acquire(void) {
    while (g_async_lock) { ws_ili9488_service(); tight_loop_contents(); }
    if (!g_bus_held) { ws_spi1_acquire_lcd(); g_bus_held = true; }
}

static inline void bus_release(void) {
    if (!g_bus_held) return;
    wait_dma();
    gpio_put(PIN_CS, 1);
    ws_spi1_release();
    g_bus_held = false;
    run_cbs();
}

static void cmd(uint8_t c) {
    wait_dma();
    gpio_put(PIN_DC, 0); gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI, &c, 1);
    gpio_put(PIN_CS, 1);
}

static void dat(const uint8_t* d, size_t n) {
    wait_dma();
    gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0);
    spi_write_blocking(SPI, d, n);
    gpio_put(PIN_CS, 1);
}

static void dat8(uint8_t v) { uint8_t b[2] = {0, v}; dat(b, 2); }

static void dma_send(const uint16_t* px, size_t bytes) {
    wait_dma();
    gpio_put(PIN_DC, 1); gpio_put(PIN_CS, 0);
    g_dma_busy = true;
    dma_channel_set_read_addr(g_dma_ch, px, false);
    dma_channel_set_trans_count(g_dma_ch, bytes, true);
}

static void set_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1) {
    wait_dma();
    cmd(CMD_CASET); dat8(x0>>8); dat8(x0&0xFF); dat8(x1>>8); dat8(x1&0xFF);
    cmd(CMD_RASET); dat8(y0>>8); dat8(y0&0xFF); dat8(y1>>8); dat8(y1&0xFF);
}

static void start_xfer(const xfer_t* t) {
    set_window(t->x, t->y, t->x + t->w - 1, t->y + t->h - 1);
    cmd(CMD_RAMWR);
    dma_send(t->px, t->bytes);
}

/* ---- Glyph lookup (shared with text functions) ---- */
static int glyph_idx(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == '.') return 36; if (c == '-') return 37; if (c == ':') return 38;
    if (c == '(') return 39; if (c == ')') return 40; if (c == '/') return 41;
    return -1;
}

/* ==== Public API ==== */

bool ws_ili9488_init(void)
{
    ws_spi1_init();

    /* Display-specific control pins */
    gpio_init(PIN_DC);  gpio_set_dir(PIN_DC, GPIO_OUT);
    gpio_init(PIN_CS);  gpio_set_dir(PIN_CS, GPIO_OUT); gpio_put(PIN_CS, 1);

    /* Backlight PWM */
    gpio_set_function(PIN_BL, GPIO_FUNC_PWM);
    uint slice = pwm_gpio_to_slice_num(PIN_BL);
    pwm_set_wrap(slice, 65535);
    pwm_set_gpio_level(PIN_BL, 65535);
    pwm_set_enabled(slice, true);

    /* DMA channel */
    g_dma_ch = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(g_dma_ch);
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_8);
    channel_config_set_dreq(&cfg, spi_get_dreq(SPI, true));
    channel_config_set_read_increment(&cfg, true);
    channel_config_set_write_increment(&cfg, false);
    dma_channel_configure(g_dma_ch, &cfg, &spi_get_hw(SPI)->dr, NULL, 0, false);

    /* Hardware reset */
    gpio_init(PIN_RST); gpio_set_dir(PIN_RST, GPIO_OUT);
    gpio_put(PIN_RST, 1); sleep_ms(10);
    gpio_put(PIN_RST, 0); sleep_ms(10);
    gpio_put(PIN_RST, 1); sleep_ms(120);

    /* ILI9488 init sequence */
    bus_acquire();
    cmd(CMD_INVON);
    cmd(CMD_PWCTR3); dat8(0x33);
    cmd(CMD_VMCTR1); dat8(0x00); dat8(0x1E); dat8(0x80);
    cmd(CMD_FRMCTR1); dat8(0xB0);
    cmd(CMD_MADCTL); dat8(0x28);
    cmd(CMD_PGAMCTRL); {
        uint8_t g[] = {0x00,0x13,0x18,0x04,0x0F,0x06,0x3A,0x56,0x4D,0x03,0x0A,0x06,0x30,0x3E,0x0F};
        dat(g, sizeof g);
    }
    cmd(CMD_NGAMCTRL); {
        uint8_t g[] = {0x00,0x13,0x18,0x01,0x11,0x06,0x38,0x34,0x4D,0x06,0x0D,0x0B,0x31,0x37,0x0F};
        dat(g, sizeof g);
    }
    cmd(CMD_COLMOD); dat8(0x55);
    cmd(CMD_DFUNCTR); dat8(0x00); dat8(0x62);
    cmd(CMD_SLPOUT); sleep_ms(120);
    cmd(CMD_DISPON); sleep_ms(10);
    bus_release();

    ws_ili9488_clear(0);
    return true;
}

void ws_ili9488_set_callback(ws_dma_callback_t cb) { g_cb = cb; }

void ws_ili9488_service(void)
{
    complete_if_done();
    if (!g_dma_busy && g_dma_queued) { g_dma_queued = false; start_xfer(&g_queued); }
    if (!g_dma_busy && !g_dma_queued && g_async_lock) {
        if (g_bus_held) { gpio_put(PIN_CS, 1); ws_spi1_release(); g_bus_held = false; }
        g_async_lock = false;
        run_cbs();
    }
}

bool ws_ili9488_is_ready(void) { return !g_dma_busy && !g_dma_queued; }

void ws_ili9488_wait(void) { wait_dma(); ws_ili9488_service(); run_cbs(); }

void ws_ili9488_clear(ws_color_t color)
{
    bus_acquire();
    set_window(0, 0, WS_LCD_WIDTH - 1, WS_LCD_HEIGHT - 1);
    cmd(CMD_RAMWR);
    for (int i = 0; i < PBUF_SIZE; i++) g_pbuf[i] = color;
    int total = WS_LCD_WIDTH * WS_LCD_HEIGHT, sent = 0;
    while (sent < total) {
        int n = total - sent; if (n > PBUF_SIZE) n = PBUF_SIZE;
        dma_send(g_pbuf, n * 2); wait_dma(); sent += n;
    }
    bus_release();
}

void ws_ili9488_fill_rect(int x, int y, int w, int h, ws_color_t color)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > WS_LCD_WIDTH)  w = WS_LCD_WIDTH  - x;
    if (y + h > WS_LCD_HEIGHT) h = WS_LCD_HEIGHT - y;
    if (w <= 0 || h <= 0) return;

    bus_acquire();
    set_window(x, y, x + w - 1, y + h - 1);
    cmd(CMD_RAMWR);
    int total = w * h, buf = total < PBUF_SIZE ? total : PBUF_SIZE;
    for (int i = 0; i < buf; i++) g_pbuf[i] = color;
    int sent = 0;
    while (sent < total) {
        int n = total - sent; if (n > buf) n = buf;
        dma_send(g_pbuf, n * 2); wait_dma(); sent += n;
    }
    bus_release();
    g_updates++;
}

void ws_ili9488_blit(int x, int y, int w, int h, const ws_color_t* px)
{
    if (!px || x < 0 || y < 0 || w <= 0 || h <= 0) return;
    if (x + w > WS_LCD_WIDTH || y + h > WS_LCD_HEIGHT) return;
    bus_acquire();
    set_window(x, y, x + w - 1, y + h - 1);
    cmd(CMD_RAMWR);
    dma_send(px, (size_t)w * h * 2); wait_dma();
    bus_release();
    g_updates++;
}

bool ws_ili9488_blit_async(int x, int y, int w, int h, const ws_color_t* px)
{
    if (!px || x < 0 || y < 0 || w <= 0 || h <= 0) return false;
    if (x + w > WS_LCD_WIDTH || y + h > WS_LCD_HEIGHT) return false;
    ws_ili9488_service();
    if (g_dma_queued) return false;

    xfer_t t = { x, y, w, h, px, (size_t)w * h * 2 };
    if (!g_dma_busy) {
        bus_acquire(); g_async_lock = true; start_xfer(&t);
    } else {
        g_queued = t; g_dma_queued = true;
    }
    g_updates++;
    return true;
}

void ws_ili9488_text(const char* s, int x, int y, ws_color_t fg, ws_color_t bg)
{
    bus_acquire();
    while (*s) {
        char c = *s++;
        if (c == ' ') { x += 6; continue; }
        int gi = glyph_idx(c);
        if (gi < 0 || gi >= FONT_GLYPH_COUNT) { x += 6; continue; }
        set_window(x, y, x + 5, y + 7); cmd(CMD_RAMWR);
        uint16_t buf[48]; int idx = 0;
        for (int r = 0; r < 8; r++)
            for (int col = 0; col < 6; col++)
                buf[idx++] = (col < 5 && (font_5x8[gi][col] & (1 << r))) ? fg : bg;
        dat((uint8_t*)buf, sizeof buf);
        x += 6;
    }
    bus_release();
}

void ws_ili9488_text_2x(const char* s, int x, int y, ws_color_t fg, ws_color_t bg)
{
    bus_acquire();
    while (*s) {
        char c = *s++;
        if (c == ' ') { x += 12; continue; }
        int gi = glyph_idx(c);
        if (gi < 0 || gi >= FONT_GLYPH_COUNT) { x += 12; continue; }
        set_window(x, y, x + 11, y + 15); cmd(CMD_RAMWR);
        uint16_t buf[192]; int idx = 0;
        for (int r = 0; r < 16; r++) {
            int sr = r / 2;
            for (int col = 0; col < 12; col++) {
                int sc = col / 2;
                buf[idx++] = (sc < 5 && (font_5x8[gi][sc] & (1 << sr))) ? fg : bg;
            }
        }
        dat((uint8_t*)buf, sizeof buf);
        x += 12;
    }
    bus_release();
}

void ws_ili9488_get_stats(uint64_t* u, uint64_t* d) {
    if (u) *u = g_updates; if (d) *d = g_completions;
}

void ws_ili9488_set_scroll_area(uint16_t tfa, uint16_t vsa, uint16_t bfa)
{
    bus_acquire();
    cmd(CMD_VSCRDEF);
    uint8_t d[6] = { tfa>>8, tfa&0xFF, vsa>>8, vsa&0xFF, bfa>>8, bfa&0xFF };
    dat(d, 6);
    bus_release();
}

void ws_ili9488_set_scroll_start(uint16_t vsp)
{
    bus_acquire();
    cmd(CMD_VSCRSADD);
    uint8_t d[2] = { vsp>>8, vsp&0xFF };
    dat(d, 2);
    bus_release();
}
