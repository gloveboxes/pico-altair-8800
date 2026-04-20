/* Waveshare 3.5" ILI9488 — Altair 8800 front-panel compositor. */

#include "ws_display.h"

#ifdef WAVESHARE_3_5_DISPLAY

#include "ws_ili9488.h"
#include "build_version.h"
#include "FrontPanels/font_5x8.h"
#include <stdio.h>
#include <string.h>

#ifdef CYW43_WL_GPIO_LED_PIN
#include "wifi.h"
#endif

/* ---- Layout constants ---- */
#define LED_SZ  18
#define SP_ST   46
#define SP_AD   29
#define SP_DA   29
#define X_ST    12
#define X_AD    6
#define X_DA    238
#define Y_TITLE 10
#define Y_SL    45
#define Y_SS    64
#define Y_SLD   72
#define Y_SN    94
#define Y_AL    110
#define Y_AS    129
#define Y_ALD   137
#define Y_AN    159
#define Y_DL    180
#define Y_DS    199
#define Y_DLD   207
#define Y_DN    229
#define Y_WIFI  290
#define RMARGIN 20

/* ---- Panel bitmap (off-screen composition buffer) ---- */
#define PA_X 0
#define PA_Y 45
#define PA_W WS_LCD_WIDTH
#define PA_H 192

static ws_color_t g_panel[PA_W * PA_H];

static inline int ly(int y) { return y - PA_Y; }
static inline int pi(int x, int y) { return y * PA_W + x; }

static void p_clear(ws_color_t c) {
    for (int i = 0; i < PA_W * PA_H; i++) g_panel[i] = c;
}

static void p_fill(int x, int y, int w, int h, ws_color_t c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > PA_W) w = PA_W - x;
    if (y + h > PA_H) h = PA_H - y;
    for (int r = 0; r < h; r++) {
        int b = pi(x, y + r);
        for (int col = 0; col < w; col++) g_panel[b + col] = c;
    }
}

static int gi(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '0' && c <= '9') return 26 + (c - '0');
    if (c == '.') return 36; if (c == '-') return 37; if (c == ':') return 38;
    if (c == '(') return 39; if (c == ')') return 40; if (c == '/') return 41;
    return -1;
}

static void p_text1(const char* s, int x, int y, ws_color_t fg, ws_color_t bg) {
    int oy = ly(y);
    while (*s) {
        char c = *s++;
        if (c == ' ') { x += 6; continue; }
        int g = gi(c);
        if (g < 0 || g >= FONT_GLYPH_COUNT) { x += 6; continue; }
        for (int r = 0; r < 8; r++) {
            int py = oy + r; if (py < 0 || py >= PA_H) continue;
            for (int col = 0; col < 6; col++) {
                int px = x + col; if (px < 0 || px >= PA_W) continue;
                ws_color_t clr = bg;
                if (col < 5 && (font_5x8[g][col] & (1 << r))) clr = fg;
                g_panel[pi(px, py)] = clr;
            }
        }
        x += 6;
    }
}

static void p_text2(const char* s, int x, int y, ws_color_t fg, ws_color_t bg) {
    int oy = ly(y);
    while (*s) {
        char c = *s++;
        if (c == ' ') { x += 12; continue; }
        int g = gi(c);
        if (g < 0 || g >= FONT_GLYPH_COUNT) { x += 12; continue; }
        for (int r = 0; r < 16; r++) {
            int py = oy + r; if (py < 0 || py >= PA_H) continue;
            for (int col = 0; col < 12; col++) {
                int px = x + col; if (px < 0 || px >= PA_W) continue;
                int sc = col / 2;
                ws_color_t clr = bg;
                if (sc < 5 && (font_5x8[g][sc] & (1 << (r / 2)))) clr = fg;
                g_panel[pi(px, py)] = clr;
            }
        }
        x += 12;
    }
}

static void p_leds(uint32_t bits, int n, int x0, int y, int sp, ws_color_t on, ws_color_t off) {
    int oy = ly(y), x = x0;
    for (int i = n - 1; i >= 0; i--) {
        p_fill(x, oy, LED_SZ, LED_SZ, (bits >> i) & 1 ? on : off);
        x += sp;
    }
}

static void compose(uint16_t addr, uint8_t data, uint16_t st)
{
    ws_color_t BG  = ws_rgb565(25, 55, 110);
    ws_color_t TXT = ws_rgb565(255, 255, 255);
    ws_color_t GRY = ws_rgb565(180, 200, 220);
    ws_color_t SEP = ws_rgb565(200, 210, 220);
    ws_color_t ON  = ws_rgb565(255, 0, 0);
    ws_color_t OFF = ws_rgb565(50, 20, 30);

    p_clear(BG);

    p_text2("STATUS",  WS_LCD_WIDTH - 6 * 12 - RMARGIN, Y_SL, TXT, BG);
    p_fill(0, ly(Y_SS), WS_LCD_WIDTH, 3, SEP);
    p_text2("ADDRESS", WS_LCD_WIDTH - 7 * 12 - RMARGIN, Y_AL, TXT, BG);
    p_fill(0, ly(Y_AS), WS_LCD_WIDTH, 3, SEP);
    p_text2("DATA",    WS_LCD_WIDTH - 4 * 12 - RMARGIN, Y_DL, TXT, BG);
    p_fill(0, ly(Y_DS), WS_LCD_WIDTH, 3, SEP);

    const char* sl[] = { "INT ","WO  ","STCK","HLTA","OUT ","M1  ","INP ","MEMR","PROT","INTE" };
    int x = X_ST;
    for (int i = 9; i >= 0; i--) {
        int tw = (int)strlen(sl[i]) * 6;
        p_text1(sl[i], x + (LED_SZ - tw) / 2, Y_SN, GRY, BG);
        x += SP_ST;
    }
    x = X_AD;
    for (int i = 15; i >= 0; i--) {
        char lb[3]; if (i >= 10) { lb[0] = '1'; lb[1] = '0' + (i - 10); lb[2] = 0; }
        else { lb[0] = '0' + i; lb[1] = 0; }
        p_text1(lb, x + 4, Y_AN, GRY, BG); x += SP_AD;
    }
    x = X_DA;
    for (int i = 7; i >= 0; i--) { char lb[2] = { '0' + i, 0 }; p_text1(lb, x + 5, Y_DN, GRY, BG); x += SP_DA; }

    p_leds(st,   10, X_ST, Y_SLD, SP_ST, ON, OFF);
    p_leds(addr, 16, X_AD, Y_ALD, SP_AD, ON, OFF);
    p_leds(data,  8, X_DA, Y_DLD, SP_DA, ON, OFF);
}

/* ==== Public API ==== */

void ws_display_init(void)
{
    if (!ws_ili9488_init()) { printf("[WS] Display init failed\n"); return; }
    printf("[WS] Display initialized (480x320, DMA, 25 MHz SD)\n");
}

void ws_display_update_wifi(const char* ssid, const char* ip)
{
#ifdef CYW43_WL_GPIO_LED_PIN
    ws_color_t W = ws_rgb565(255, 255, 255);
    ws_color_t BG = ws_rgb565(25, 55, 110);
    (void)ssid;
    ws_ili9488_fill_rect(0, Y_WIFI - 2, WS_LCD_WIDTH, 18, BG);
    if (ip && ip[0]) {
        char buf[96];
        const char* h = wifi_get_hostname();
        if (h && h[0]) snprintf(buf, sizeof buf, "WIFI: %s | %s.local", ip, h);
        else           snprintf(buf, sizeof buf, "WIFI: %s", ip);
        ws_ili9488_text(buf, 6, Y_WIFI, W, BG);
    }
#else
    (void)ssid; (void)ip;
#endif
}

void ws_display_init_front_panel(void)
{
    ws_color_t W  = ws_rgb565(255, 255, 255);
    ws_color_t BG = ws_rgb565(25, 55, 110);
    ws_ili9488_clear(BG);
    char title[64];
    snprintf(title, sizeof title, "ALTAIR 8800 (%d %s %s)", BUILD_VERSION, BUILD_DATE, BUILD_TIME);
    ws_ili9488_text_2x(title, 4, Y_TITLE, W, BG);

#ifdef CYW43_WL_GPIO_LED_PIN
    const char* ip = wifi_get_ip_address();
    if (ip && ip[0]) {
        char buf[96];
        const char* h = wifi_get_hostname();
        if (h && h[0]) snprintf(buf, sizeof buf, "WIFI: %s | %s.local", ip, h);
        else           snprintf(buf, sizeof buf, "WIFI: %s", ip);
        ws_ili9488_text(buf, 6, Y_WIFI, W, BG);
    }
#endif

    compose(0, 0, 0);
    ws_ili9488_blit(PA_X, PA_Y, PA_W, PA_H, g_panel);
    printf("[WS] Front panel initialized\n");
}

void ws_display_show_front_panel(uint16_t address, uint8_t data, uint16_t status)
{
    static uint16_t ls = 0xFFFF, la = 0xFFFF;
    static uint8_t  ld = 0xFF;
    ws_ili9488_service();
    if (status == ls && address == la && data == ld) return;
    if (!ws_ili9488_is_ready()) return;
    compose(address, data, status);
    if (!ws_ili9488_blit_async(PA_X, PA_Y, PA_W, PA_H, g_panel)) return;
    ls = status; la = address; ld = data;
}

void ws_display_poll(void) { ws_ili9488_service(); }

#endif /* WAVESHARE_3_5_DISPLAY */
