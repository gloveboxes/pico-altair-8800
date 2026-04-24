/* VT100 terminal emulator for ILI9488 480×320 display.
 *
 * 80×30 terminal using 6×10 font (300px).  Bottom 20px is a status bar
 * showing IP address and condensed CPU status/address/data LEDs.
 * Software scroll with row-buffer rendering.
 * ANSI 16-colour support (SGR 30–37, 40–47, bold/bright 90–97, 100–107).
 * xterm 256-colour SGR (38;5;n and 48;5;n) is mapped to the nearest
 * 16-colour palette entry to keep per-cell storage compact.
 * Escape sequence state machine handles byte-at-a-time input.
 *
 * All drawing happens on core 1 via ws_ili9488 (shared SPI1 bus).
 */

#include "vt100_display.h"
#include "font_6x10.h"
#include "drivers/waveshare/ws_ili9488.h"

#include <string.h>
#include <stdio.h>

/* ---- Geometry ---- */
#define CHAR_W   VT_FONT_W    /* 6 */
#define CHAR_H   VT_FONT_H    /* 10 */
#define DISP_W   WS_LCD_WIDTH  /* 480 */
#define DISP_H   WS_LCD_HEIGHT /* 320 */
#define TERM_H   (VT_ROWS * CHAR_H) /* 300 — terminal area */
#define BAR_H    (DISP_H - TERM_H)  /* 20 — status bar */
#define BAR_Y    TERM_H              /* 300 */

_Static_assert(VT_COLS * CHAR_W == DISP_W, "80 cols × 6px = 480");
_Static_assert(VT_ROWS * CHAR_H + BAR_H == DISP_H, "30 rows + 20px bar = 320");

/* ---- ANSI 16-colour palette (standard + bright) ---- */
static const ws_color_t ansi_palette[16] = {
    /* 0 Black   */ 0x0000,
    /* 1 Red     */ 0x00F8,  /* ws_rgb565(255,0,0) byte-swapped */
    /* 2 Green   */ 0xE007,  /* ws_rgb565(0,255,0) */
    /* 3 Yellow  */ 0xE0FF,  /* ws_rgb565(255,255,0) */
    /* 4 Blue    */ 0x1F00,  /* ws_rgb565(0,0,255) */
    /* 5 Magenta */ 0x1FF8,  /* ws_rgb565(255,0,255) */
    /* 6 Cyan    */ 0xFF07,  /* ws_rgb565(0,255,255) */
    /* 7 White   */ 0xFFFF,  /* ws_rgb565(255,255,255) */
    /* 8  Bright black (grey) */  0x0000, /* filled at init */
    /* 9  Bright red    */        0x0000,
    /* 10 Bright green  */        0x0000,
    /* 11 Bright yellow */        0x0000,
    /* 12 Bright blue   */        0x0000,
    /* 13 Bright magenta*/        0x0000,
    /* 14 Bright cyan   */        0x0000,
    /* 15 Bright white  */        0x0000,
};

/* Mutable copy filled at init so ws_rgb565 is called at runtime. */
static ws_color_t palette[16];

/* Filled 6x10 oval used for bright-white 'O' game balls on the TFT. */
static const uint16_t ball_glyph[VT_FONT_W] = {
    0x1FE, 0x3FF, 0x3FF, 0x3FF, 0x3FF, 0x1FE
};

/* ---- Per-cell attributes ---- */
typedef struct {
    uint8_t ch;       /* ASCII character (or 0 for space) */
    uint8_t fg : 4;   /* palette index */
    uint8_t bg : 4;   /* palette index */
} cell_t;

/* ---- Terminal state ---- */
static cell_t  screen[VT_ROWS][VT_COLS];
static bool    dirty[VT_ROWS][VT_COLS];
static int     cursor_x, cursor_y;       /* column, row (0-based) */
static int     saved_cx, saved_cy;       /* saved cursor position */
static int     prev_cx, prev_cy;         /* previous cursor position for undraw */
static bool    cursor_visible;           /* whether cursor should be shown */
static bool    cursor_drawn;             /* whether cursor is currently on screen */
static uint8_t cur_fg, cur_bg;           /* current colour attrs */
static bool    bold;                     /* bold / bright flag */

/* ---- Row dirty flags (true = at least one cell in row is dirty) ---- */
static bool    row_dirty[VT_ROWS];

/* ---- Status bar state ---- */
static uint16_t bar_address;
static uint8_t  bar_data;
static uint16_t bar_status;
static char     bar_ip[32];
static bool     bar_dirty;

/* Status bar pixel buffer — 480×20 = 9600 pixels (19.2 KB) */
static ws_color_t bar_buf[DISP_W * BAR_H];

/* ---- Escape sequence parser ---- */
typedef enum {
    ST_NORMAL,
    ST_ESC,          /* received ESC (0x1B) */
    ST_CSI,          /* received ESC [ */
    ST_CSI_PARAM,    /* collecting numeric params */
    ST_CSI_INTER,    /* intermediate bytes (0x20-0x2F) — mostly ignored */
    ST_ESC_OTHER,    /* ESC followed by non-'[' — consume one more byte */
} parse_state_t;

static parse_state_t pstate;
static bool csi_private;     /* true when '?' prefix seen */
#define MAX_CSI_PARAMS 8
static int  csi_params[MAX_CSI_PARAMS];
static int  csi_nparam;
static bool csi_has_digit;

/* ---- Row line buffer for blitting one character cell ---- */
static ws_color_t cell_buf[CHAR_W * CHAR_H]; /* 6×10 = 60 pixels */

/* ---- Row batch buffer for rendering up to BATCH_ROWS rows (480×100 = 48000 pixels) ---- */
#define BATCH_ROWS 10
static ws_color_t row_buf[DISP_W * CHAR_H * BATCH_ROWS];

/* ---- Forward declarations ---- */
static void draw_cell(int col, int row);
static void draw_cell_inv(int col, int row);
static void draw_row(int row);
static void scroll_up(void);
static void clear_row(int row);
static void clear_row_range(int row, int col_start, int col_end);
static void csi_dispatch(uint8_t final_ch);
static void sgr(void);
static uint8_t xterm_to_ansi(int code);
static bool is_ball_cell(cell_t c);
static void compose_status_bar(void);
static void bar_draw_text_col(const char* s, int x, int y, ws_color_t fg);

/* ==================================================================== */
/* Palette init                                                         */
/* ==================================================================== */

static void init_palette(void)
{
    palette[0]  = ws_rgb565(0,   0,   0);
    palette[1]  = ws_rgb565(170, 0,   0);
    palette[2]  = ws_rgb565(0,   170, 0);
    palette[3]  = ws_rgb565(170, 85,  0);
    palette[4]  = ws_rgb565(0,   0,   170);
    palette[5]  = ws_rgb565(170, 0,   170);
    palette[6]  = ws_rgb565(0,   170, 170);
    palette[7]  = ws_rgb565(170, 170, 170);
    palette[8]  = ws_rgb565(85,  85,  85);
    palette[9]  = ws_rgb565(255, 85,  85);
    palette[10] = ws_rgb565(85,  255, 85);
    palette[11] = ws_rgb565(255, 255, 85);
    palette[12] = ws_rgb565(85,  85,  255);
    palette[13] = ws_rgb565(255, 85,  255);
    palette[14] = ws_rgb565(85,  255, 255);
    palette[15] = ws_rgb565(255, 255, 255);
}

/* ==================================================================== */
/* Drawing                                                              */
/* ==================================================================== */

static bool is_ball_cell(cell_t c)
{
    return c.ch == 'O' && c.fg == 15 && c.bg == 0;
}

static void draw_cell(int col, int row)
{
    if (col < 0 || col >= VT_COLS || row < 0 || row >= VT_ROWS) return;

    cell_t c = screen[row][col];
    uint8_t ch = c.ch;
    ws_color_t fg = palette[c.fg];
    ws_color_t bg = palette[c.bg];

    /* Build pixel buffer for this cell */
    int gi = (ch >= VT_FONT_FIRST && ch <= VT_FONT_LAST) ? (ch - VT_FONT_FIRST) : 0;
    const uint16_t* glyph = is_ball_cell(c) ? ball_glyph : font_6x10[gi];

    int idx = 0;
    for (int r = 0; r < CHAR_H; r++) {
        for (int cx = 0; cx < CHAR_W; cx++) {
            cell_buf[idx++] = (glyph[cx] & (1 << r)) ? fg : bg;
        }
    }

    ws_ili9488_blit(col * CHAR_W, row * CHAR_H, CHAR_W, CHAR_H, cell_buf);
    dirty[row][col] = false;
}

/* Draw a single cell with inverted colours (for cursor). */
static void draw_cell_inv(int col, int row)
{
    if (col < 0 || col >= VT_COLS || row < 0 || row >= VT_ROWS) return;

    cell_t c = screen[row][col];
    uint8_t ch = c.ch;
    ws_color_t fg = palette[c.bg];  /* swap fg/bg */
    ws_color_t bg = palette[c.fg];

    int gi = (ch >= VT_FONT_FIRST && ch <= VT_FONT_LAST) ? (ch - VT_FONT_FIRST) : 0;
    const uint16_t* glyph = font_6x10[gi];

    int idx = 0;
    for (int r = 0; r < CHAR_H; r++) {
        for (int cx = 0; cx < CHAR_W; cx++) {
            cell_buf[idx++] = (glyph[cx] & (1 << r)) ? fg : bg;
        }
    }

    ws_ili9488_blit(col * CHAR_W, row * CHAR_H, CHAR_W, CHAR_H, cell_buf);
}

/* Render a single row into row_buf at a vertical offset within the batch buffer. */
static void compose_row(int row, int batch_offset)
{
    for (int c = 0; c < VT_COLS; c++) {
        cell_t cell = screen[row][c];
        ws_color_t fg = palette[cell.fg];
        ws_color_t bg = palette[cell.bg];
        int gi = (cell.ch >= VT_FONT_FIRST && cell.ch <= VT_FONT_LAST)
                     ? (cell.ch - VT_FONT_FIRST) : 0;
        const uint16_t* glyph = is_ball_cell(cell) ? ball_glyph : font_6x10[gi];

        int x0 = c * CHAR_W;
        for (int r = 0; r < CHAR_H; r++) {
            int base = (batch_offset * CHAR_H + r) * DISP_W + x0;
            for (int cx = 0; cx < CHAR_W; cx++) {
                row_buf[base + cx] = (glyph[cx] & (1 << r)) ? fg : bg;
            }
        }
        dirty[row][c] = false;
    }
    row_dirty[row] = false;
}

/* Render a single row (480×10) and blit immediately. */
static void draw_row(int row)
{
    if (row < 0 || row >= VT_ROWS) return;
    compose_row(row, 0);
    ws_ili9488_blit(0, row * CHAR_H, DISP_W, CHAR_H, row_buf);
}

static void clear_row(int row)
{
    for (int c = 0; c < VT_COLS; c++) {
        screen[row][c].ch = ' ';
        screen[row][c].fg = 7;  /* white */
        screen[row][c].bg = 0;  /* black */
        dirty[row][c] = true;
    }
    row_dirty[row] = true;
}

static void clear_row_range(int row, int col_start, int col_end)
{
    if (col_start < 0) col_start = 0;
    if (col_end > VT_COLS) col_end = VT_COLS;
    for (int c = col_start; c < col_end; c++) {
        screen[row][c].ch = ' ';
        screen[row][c].fg = cur_fg;
        screen[row][c].bg = cur_bg;
        dirty[row][c] = true;
    }
    row_dirty[row] = true;
}

/* ==================================================================== */
/* Software scroll                                                      */
/* ==================================================================== */

static void scroll_up(void)
{
    /* Shift entire screen buffer up by one row in a single operation. */
    memmove(screen[0], screen[1], sizeof(cell_t) * VT_COLS * (VT_ROWS - 1));

    /* Mark all shifted rows dirty. */
    memset(dirty, true, sizeof(dirty));
    memset(row_dirty, true, sizeof(row_dirty));

    /* Clear the new bottom row. */
    clear_row(VT_ROWS - 1);
}

/* ==================================================================== */
/* CSI dispatch (ESC [ ... <final>)                                     */
/* ==================================================================== */

static inline int p(int idx, int def)
{
    return (idx < csi_nparam && csi_params[idx] > 0) ? csi_params[idx] : def;
}

static void csi_dispatch(uint8_t ch)
{
    int n, m;

    switch (ch) {
    /* ---- Cursor movement ---- */
    case 'A': /* CUU — cursor up */
        n = p(0, 1);
        cursor_y -= n; if (cursor_y < 0) cursor_y = 0;
        break;
    case 'B': /* CUD — cursor down */
        n = p(0, 1);
        cursor_y += n; if (cursor_y >= VT_ROWS) cursor_y = VT_ROWS - 1;
        break;
    case 'C': /* CUF — cursor forward */
        n = p(0, 1);
        cursor_x += n; if (cursor_x >= VT_COLS) cursor_x = VT_COLS - 1;
        break;
    case 'D': /* CUB — cursor back */
        n = p(0, 1);
        cursor_x -= n; if (cursor_x < 0) cursor_x = 0;
        break;
    case 'H': /* CUP — cursor position */
    case 'f': /* HVP — same as CUP */
        cursor_y = p(0, 1) - 1;
        cursor_x = p(1, 1) - 1;
        if (cursor_y < 0) cursor_y = 0;
        if (cursor_y >= VT_ROWS) cursor_y = VT_ROWS - 1;
        if (cursor_x < 0) cursor_x = 0;
        if (cursor_x >= VT_COLS) cursor_x = VT_COLS - 1;
        break;

    /* ---- Erase ---- */
    case 'J': /* ED — erase in display */
        n = p(0, 0);
        if (n == 0) { /* cursor to end */
            clear_row_range(cursor_y, cursor_x, VT_COLS);
            for (int r = cursor_y + 1; r < VT_ROWS; r++) {
                clear_row_range(r, 0, VT_COLS);
            }
        } else if (n == 1) { /* start to cursor */
            for (int r = 0; r < cursor_y; r++) {
                clear_row_range(r, 0, VT_COLS);
            }
            clear_row_range(cursor_y, 0, cursor_x + 1);
        } else if (n == 2) { /* entire screen */
            for (int r = 0; r < VT_ROWS; r++) {
                clear_row_range(r, 0, VT_COLS);
            }
            cursor_x = 0; cursor_y = 0; /* home cursor (xterm/VT220 behavior) */
        }
        break;
    case 'K': /* EL — erase in line */
        n = p(0, 0);
        if (n == 0) {
            clear_row_range(cursor_y, cursor_x, VT_COLS);
        } else if (n == 1) {
            clear_row_range(cursor_y, 0, cursor_x + 1);
        } else if (n == 2) {
            clear_row_range(cursor_y, 0, VT_COLS);
        }
        break;

    /* ---- Scroll ---- */
    case 'S': /* SU — scroll up */
        n = p(0, 1);
        for (int i = 0; i < n; i++) scroll_up();
        break;

    /* ---- Insert/Delete lines ---- */
    case 'L': /* IL — insert lines */
        n = p(0, 1);
        for (int i = 0; i < n && cursor_y + n < VT_ROWS; i++) {
            /* Shift rows down from bottom. */
            for (int r = VT_ROWS - 1; r > cursor_y; r--) {
                memcpy(screen[r], screen[r - 1], sizeof(cell_t) * VT_COLS);
                memset(dirty[r], true, sizeof(bool) * VT_COLS);
            }
            clear_row_range(cursor_y, 0, VT_COLS);
        }
        break;
    case 'M': /* DL — delete lines */
        n = p(0, 1);
        for (int i = 0; i < n; i++) {
            for (int r = cursor_y; r < VT_ROWS - 1; r++) {
                memcpy(screen[r], screen[r + 1], sizeof(cell_t) * VT_COLS);
                memset(dirty[r], true, sizeof(bool) * VT_COLS);
            }
            clear_row_range(VT_ROWS - 1, 0, VT_COLS);
        }
        break;

    /* ---- SGR — Select Graphic Rendition ---- */
    case 'm':
        sgr();
        break;

    /* ---- Cursor save/restore ---- */
    case 's':
        saved_cx = cursor_x; saved_cy = cursor_y;
        break;
    case 'u':
        cursor_x = saved_cx; cursor_y = saved_cy;
        break;

    /* ---- Device status report ---- */
    case 'n':
        /* DSR — we don't respond, just ignore. */
        break;

    /* ---- Insert/delete characters ---- */
    case '@': /* ICH — insert characters */
        n = p(0, 1);
        if (n > VT_COLS - cursor_x) n = VT_COLS - cursor_x;
        for (int c = VT_COLS - 1; c >= cursor_x + n; c--) {
            screen[cursor_y][c] = screen[cursor_y][c - n];
            dirty[cursor_y][c] = true;
        }
        clear_row_range(cursor_y, cursor_x, cursor_x + n);
        break;
    case 'P': /* DCH — delete characters */
        n = p(0, 1);
        if (n > VT_COLS - cursor_x) n = VT_COLS - cursor_x;
        for (int c = cursor_x; c < VT_COLS - n; c++) {
            screen[cursor_y][c] = screen[cursor_y][c + n];
            dirty[cursor_y][c] = true;
        }
        clear_row_range(cursor_y, VT_COLS - n, VT_COLS);
        break;

    default:
        /* Unknown CSI sequence — ignore. */
        break;
    }
}

/* ---- Private mode dispatch (ESC [ ? ... h/l) ---- */
static void csi_private_dispatch(uint8_t ch)
{
    int n = p(0, 0);
    if (ch == 'h') {
        /* DECSET — set mode */
        if (n == 25) {
            cursor_visible = true;
        }
    } else if (ch == 'l') {
        /* DECRST — reset mode */
        if (n == 25) {
            cursor_visible = false;
            if (cursor_drawn) {
                draw_cell(prev_cx, prev_cy);
                cursor_drawn = false;
            }
        }
    }
}

/* ---- SGR (Select Graphic Rendition) ---- */
static uint8_t xterm_to_ansi(int code)
{
    int r, g, b, max, bright;

    if (code < 0) return 7;
    if (code < 16) return (uint8_t)code;

    if (code >= 232) {
        if (code < 244) return 8;
        if (code < 252) return 7;
        return 15;
    }

    if (code > 231) return 7;

    code -= 16;
    r = code / 36;
    g = (code / 6) % 6;
    b = code % 6;
    max = r;
    if (g > max) max = g;
    if (b > max) max = b;
    bright = (max >= 4) ? 8 : 0;

    if (max == 0) return 0;
    if (r >= 4 && g >= 2 && g <= 3 && b <= 1) return 3;
    if (r >= 4 && g >= 4 && b <= 2) return 3 + bright;
    if (r >= 4 && b >= 3 && g <= 2) return 5 + bright;
    if (g >= 4 && b >= 3 && r <= 2) return 6 + bright;
    if (r >= 4 && g >= 4 && b >= 4) return 7 + bright;
    if (r >= g && r >= b) return 1 + bright;
    if (g >= r && g >= b) return 2 + bright;
    return 4 + bright;
}

static void sgr(void)
{
    if (csi_nparam == 0) {
        /* ESC[m — reset all attributes */
        cur_fg = 7; cur_bg = 0; bold = false;
        return;
    }
    for (int i = 0; i < csi_nparam; i++) {
        int v = csi_params[i];
        if (v == 0) { cur_fg = 7; cur_bg = 0; bold = false; }
        else if (v == 1) { bold = true; if (cur_fg < 8) cur_fg += 8; }
        else if (v == 22) { bold = false; if (cur_fg >= 8) cur_fg -= 8; }
        else if (v == 7) { /* reverse video */ uint8_t t = cur_fg; cur_fg = cur_bg; cur_bg = t; }
        else if (v >= 30 && v <= 37) { cur_fg = (v - 30) + (bold ? 8 : 0); }
        else if (v >= 40 && v <= 47) { cur_bg = v - 40; }
        else if (v == 39) { cur_fg = 7 + (bold ? 8 : 0); } /* default fg */
        else if (v == 49) { cur_bg = 0; }                    /* default bg */
        else if (v >= 90  && v <= 97)  { cur_fg = v - 90 + 8; }  /* bright fg */
        else if (v >= 100 && v <= 107) { cur_bg = v - 100 + 8; } /* bright bg */
        else if ((v == 38 || v == 48) && i + 2 < csi_nparam && csi_params[i + 1] == 5) {
            uint8_t mapped = xterm_to_ansi(csi_params[i + 2]);
            if (v == 38) cur_fg = mapped;
            else cur_bg = mapped;
            i += 2;
        }
    }
}

/* ==================================================================== */
/* Escape sequence state machine                                        */
/* ==================================================================== */

static void reset_parser(void)
{
    pstate = ST_NORMAL;
    csi_nparam = 0;
    csi_has_digit = false;
    csi_private = false;
    memset(csi_params, 0, sizeof(csi_params));
}

static void emit_char(uint8_t ch)
{
    if (cursor_x >= VT_COLS) {
        /* Auto-wrap */
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= VT_ROWS) {
            cursor_y = VT_ROWS - 1;
            scroll_up();
        }
    }

    screen[cursor_y][cursor_x].ch = ch;
    screen[cursor_y][cursor_x].fg = cur_fg;
    screen[cursor_y][cursor_x].bg = cur_bg;
    dirty[cursor_y][cursor_x] = true;
    row_dirty[cursor_y] = true;
    cursor_x++;
}

void vt100_putchar(uint8_t c)
{
    switch (pstate) {
    case ST_NORMAL:
        if (c == 0x1B) {
            pstate = ST_ESC;
        } else if (c == '\r') {
            cursor_x = 0;
        } else if (c == '\n') {
            cursor_y++;
            if (cursor_y >= VT_ROWS) {
                cursor_y = VT_ROWS - 1;
                scroll_up();
            }
        } else if (c == '\b') {
            if (cursor_x > 0) cursor_x--;
        } else if (c == '\t') {
            /* Tab to next 8-column stop */
            int next = (cursor_x + 8) & ~7;
            if (next >= VT_COLS) next = VT_COLS - 1;
            cursor_x = next;
        } else if (c == '\a') {
            /* Bell — ignore */
        } else if (c >= 0x20 && c <= 0x7E) {
            emit_char(c);
        }
        /* else: ignore other control chars */
        break;

    case ST_ESC:
        if (c == '[') {
            pstate = ST_CSI;
            csi_nparam = 0;
            csi_has_digit = false;
            csi_private = false;
            memset(csi_params, 0, sizeof(csi_params));
        } else if (c == '(' || c == ')' || c == '#') {
            /* ESC ( / ESC ) / ESC # — consume one more byte. */
            pstate = ST_ESC_OTHER;
        } else if (c == 'D') {
            /* IND — index (move down, scroll if at bottom) */
            cursor_y++;
            if (cursor_y >= VT_ROWS) { cursor_y = VT_ROWS - 1; scroll_up(); }
            pstate = ST_NORMAL;
        } else if (c == 'M') {
            /* RI — reverse index (move up, scroll if at top) */
            cursor_y--;
            if (cursor_y < 0) {
                cursor_y = 0;
                /* Reverse scroll — shift everything down, clear top row. */
                for (int r = VT_ROWS - 1; r > 0; r--) {
                    memcpy(screen[r], screen[r - 1], sizeof(cell_t) * VT_COLS);
                    memset(dirty[r], true, sizeof(bool) * VT_COLS);
                }
                clear_row_range(0, 0, VT_COLS);
            }
            pstate = ST_NORMAL;
        } else if (c == 'c') {
            /* RIS — full reset */
            vt100_init();
        } else if (c == '7') {
            /* DECSC — save cursor */
            saved_cx = cursor_x; saved_cy = cursor_y;
            pstate = ST_NORMAL;
        } else if (c == '8') {
            /* DECRC — restore cursor */
            cursor_x = saved_cx; cursor_y = saved_cy;
            pstate = ST_NORMAL;
        } else {
            /* Unknown ESC sequence — ignore and return to normal. */
            pstate = ST_NORMAL;
        }
        break;

    case ST_ESC_OTHER:
        /* Consume the byte after ESC ( / ESC ) / ESC # and return. */
        pstate = ST_NORMAL;
        break;

    case ST_CSI:
    case ST_CSI_PARAM:
        if (c >= '0' && c <= '9') {
            pstate = ST_CSI_PARAM;
            if (csi_nparam == 0) csi_nparam = 1;
            csi_params[csi_nparam - 1] = csi_params[csi_nparam - 1] * 10 + (c - '0');
            csi_has_digit = true;
        } else if (c == ';') {
            pstate = ST_CSI_PARAM;
            if (csi_nparam == 0) csi_nparam = 1; /* implicit first param = 0 */
            if (csi_nparam < MAX_CSI_PARAMS) csi_nparam++;
            csi_has_digit = false;
        } else if (c >= 0x20 && c <= 0x2F) {
            /* Intermediate bytes — flag but mostly ignore. */
            pstate = ST_CSI_INTER;
        } else if (c == '?') {
            /* Private mode prefix — track and keep parsing params. */
            csi_private = true;
            pstate = ST_CSI_PARAM;
        } else if (c >= 0x40 && c <= 0x7E) {
            /* Final byte — dispatch. */
            if (csi_private)
                csi_private_dispatch(c);
            else
                csi_dispatch(c);
            csi_private = false;
            pstate = ST_NORMAL;
        } else {
            /* Unexpected — abort sequence. */
            csi_private = false;
            pstate = ST_NORMAL;
        }
        break;

    case ST_CSI_INTER:
        if (c >= 0x40 && c <= 0x7E) {
            /* Final byte after intermediates — ignore entire sequence. */
            pstate = ST_NORMAL;
        } else if (c < 0x20 || c > 0x2F) {
            /* Unexpected — abort. */
            pstate = ST_NORMAL;
        }
        break;
    }
}

/* ==================================================================== */
/* Service — flush dirty cells to display                               */
/* ==================================================================== */

void vt100_service(void)
{
    ws_ili9488_service();

    /* Undraw cursor if it moved or content underneath changed. */
    if (cursor_drawn &&
        (prev_cx != cursor_x || prev_cy != cursor_y ||
         dirty[prev_cy][prev_cx] || row_dirty[prev_cy])) {
        /* Redraw old cursor cell normally (if not about to be redrawn by dirty). */
        if (!dirty[prev_cy][prev_cx] && !row_dirty[prev_cy]) {
            draw_cell(prev_cx, prev_cy);
        }
        cursor_drawn = false;
    }

    /* Count dirty rows. */
    int dirty_count = 0;
    for (int r = 0; r < VT_ROWS; r++)
        if (row_dirty[r]) dirty_count++;

    if (dirty_count == 0) {
        /* No full-row dirty — check for individual dirty cells. */
        for (int r = 0; r < VT_ROWS; r++) {
            for (int c = 0; c < VT_COLS; c++) {
                if (dirty[r][c]) {
                    draw_cell(c, r);
                    return;
                }
            }
        }
    } else {
        /* Batch consecutive dirty rows into single blits (up to BATCH_ROWS). */
        int r = 0;
        while (r < VT_ROWS) {
            if (!row_dirty[r]) { r++; continue; }
            /* Find run of consecutive dirty rows, capped at BATCH_ROWS. */
            int start = r;
            int count = 0;
            while (r < VT_ROWS && row_dirty[r] && count < BATCH_ROWS) {
                compose_row(r, count);
                count++;
                r++;
            }
            ws_ili9488_blit(0, start * CHAR_H, DISP_W, CHAR_H * count, row_buf);
        }
    }

    /* Draw cursor at current position. */
    if (cursor_visible && !cursor_drawn) {
        draw_cell_inv(cursor_x, cursor_y);
        prev_cx = cursor_x;
        prev_cy = cursor_y;
        cursor_drawn = true;
    }

    /* Render status bar if dirty. */
    if (bar_dirty) {
        bar_dirty = false;
        compose_status_bar();
        ws_ili9488_blit(0, BAR_Y, DISP_W, BAR_H, bar_buf);
    }
}

void vt100_service_status_bar(void)
{
    ws_ili9488_service();
    if (bar_dirty) {
        bar_dirty = false;
        compose_status_bar();
        ws_ili9488_blit(0, BAR_Y, DISP_W, BAR_H, bar_buf);
    }
}

bool vt100_is_idle(void)
{
    if (!ws_ili9488_is_ready()) return false;
    for (int r = 0; r < VT_ROWS; r++)
        for (int c = 0; c < VT_COLS; c++)
            if (dirty[r][c]) return false;
    return true;
}

/* ==================================================================== */
/* Status bar rendering                                                 */
/* ==================================================================== */

/* LED geometry: 4×4 pixels, 6px pitch (2px gap) */
#define LED_SZ   4
#define LED_PITCH 6

/* Right-aligned LED positions:
 * Row 1: Status (10 LEDs) | 8px gap | Data (8 LEDs)
 * Row 2: Address (16 LEDs) right-aligned to data end
 */
#define DATA_LEDS   8
#define STATUS_LEDS 10
#define ADDR_LEDS   16
#define LED_GAP     8   /* gap between status and data groups */

/* Data LEDs end at right edge with 4px margin */
#define DATA_X0     (DISP_W - 4 - (DATA_LEDS * LED_PITCH - (LED_PITCH - LED_SZ)))
#define STATUS_X0   (DATA_X0 - LED_GAP - (STATUS_LEDS * LED_PITCH - (LED_PITCH - LED_SZ)))
/* Address LEDs right-aligned to same right edge as data */
#define ADDR_X0     (DISP_W - 4 - (ADDR_LEDS * LED_PITCH - (LED_PITCH - LED_SZ)))

/* Vertical centering: LEDs centered in each 10px row */
#define LED_ROW1_Y  3   /* offset within bar_buf for row 1 */
#define LED_ROW2_Y  13  /* offset within bar_buf for row 2 */

/* Bar colours */
static ws_color_t bar_bg;      /* dark panel background */
static ws_color_t bar_sep;     /* separator line */
static ws_color_t led_on;      /* LED on (red) */
static ws_color_t led_off;     /* LED off (dark) */
static ws_color_t bar_text;    /* IP text colour */
static ws_color_t bar_title;   /* ALTAIR 8800 title colour */
static ws_color_t bar_sub;     /* COMPUTER subtitle colour */

static void bar_fill(int x, int y, int w, int h, ws_color_t c)
{
    for (int r = 0; r < h; r++) {
        int base = (y + r) * DISP_W + x;
        for (int cx = 0; cx < w; cx++)
            bar_buf[base + cx] = c;
    }
}

static void bar_draw_leds(uint32_t bits, int n, int x0, int y0)
{
    for (int i = n - 1; i >= 0; i--) {
        ws_color_t c = (bits >> i) & 1 ? led_on : led_off;
        bar_fill(x0, y0, LED_SZ, LED_SZ, c);
        x0 += LED_PITCH;
    }
}

/* Render a text string into bar_buf using the 6×10 VT100 font. */
static void bar_draw_text(const char* s, int x, int y)
{
    bar_draw_text_col(s, x, y, bar_text);
}

static void bar_draw_text_col(const char* s, int x, int y, ws_color_t fg)
{
    while (*s) {
        char ch = *s++;
        if (ch < VT_FONT_FIRST || ch > VT_FONT_LAST) { x += CHAR_W; continue; }
        int gi = ch - VT_FONT_FIRST;
        const uint16_t* glyph = font_6x10[gi];
        for (int r = 0; r < CHAR_H && (y + r) < BAR_H; r++) {
            int base = (y + r) * DISP_W + x;
            for (int cx = 0; cx < CHAR_W; cx++) {
                if (x + cx >= 0 && x + cx < DISP_W)
                    bar_buf[base + cx] = (glyph[cx] & (1 << r)) ? fg : bar_bg;
            }
        }
        x += CHAR_W;
    }
}

static void compose_status_bar(void)
{
    /* Fill background */
    for (int i = 0; i < DISP_W * BAR_H; i++) bar_buf[i] = bar_bg;

    /* 1px separator line at top */
    for (int x = 0; x < DISP_W; x++) bar_buf[x] = bar_sep;

    /* Row 1: IP text (left), "ALTAIR 8800" (center), Status+Data LEDs (right) */
    if (bar_ip[0]) {
        bar_draw_text(bar_ip, 4, 5);
    }

    /* Center title between IP area and LED area */
    #define TITLE_STR   "ALTAIR 8800"
    #define SUB_STR     "COMPUTER"
    #define TITLE_LEN   11  /* strlen("ALTAIR 8800") */
    #define SUB_LEN     8   /* strlen("COMPUTER") */
    #define MID_X       ((STATUS_X0 + 100) / 2)  /* center between IP and LEDs */
    bar_draw_text_col(TITLE_STR, MID_X - (TITLE_LEN * CHAR_W) / 2, 1, bar_title);
    bar_draw_text_col(SUB_STR,   MID_X - (SUB_LEN * CHAR_W) / 2,  10, bar_sub);

    bar_draw_leds(bar_status, STATUS_LEDS, STATUS_X0, LED_ROW1_Y);
    bar_draw_leds(bar_data,   DATA_LEDS,   DATA_X0,   LED_ROW1_Y);

    /* Row 2: Address LEDs (right-aligned) */
    bar_draw_leds(bar_address, ADDR_LEDS, ADDR_X0, LED_ROW2_Y);
}

void vt100_update_status(uint16_t address, uint8_t data, uint16_t status)
{
    if (address == bar_address && data == bar_data && status == bar_status)
        return;
    bar_address = address;
    bar_data = data;
    bar_status = status;
    bar_dirty = true;
}

void vt100_set_ip(const char* ip)
{
    if (ip) {
        strncpy(bar_ip, ip, sizeof(bar_ip) - 1);
        bar_ip[sizeof(bar_ip) - 1] = '\0';
    } else {
        bar_ip[0] = '\0';
    }
    bar_dirty = true;
}

/* ==================================================================== */
/* Init                                                                 */
/* ==================================================================== */

void vt100_init(void)
{
    init_palette();
    reset_parser();

    cursor_x = 0; cursor_y = 0;
    saved_cx = 0; saved_cy = 0;
    prev_cx = 0;  prev_cy = 0;
    cursor_visible = true;
    cursor_drawn = false;
    cur_fg = 7;   cur_bg = 0;
    bold = false;

    /* Status bar colours */
    bar_bg   = ws_rgb565(25,  45,  80);   /* dark blue-grey panel */
    bar_sep  = ws_rgb565(80, 100, 130);   /* separator line */
    led_on   = ws_rgb565(255, 30,  20);   /* bright red LED */
    led_off  = ws_rgb565(50,  20,  25);   /* dim LED */
    bar_text = ws_rgb565(180, 200, 220);  /* light grey text */
    bar_title = ws_rgb565(255, 255, 255);  /* bright white for ALTAIR 8800 */
    bar_sub   = ws_rgb565(140, 160, 180);  /* muted grey for COMPUTER */

    /* Clear status bar state */
    bar_address = 0; bar_data = 0; bar_status = 0;
    bar_ip[0] = '\0';
    bar_dirty = true;

    /* Clear screen buffer. */
    for (int r = 0; r < VT_ROWS; r++) clear_row(r);

    /* Clear physical display. */
    ws_ili9488_clear(palette[0]);

    /* Render initial status bar. */
    compose_status_bar();
    ws_ili9488_blit(0, BAR_Y, DISP_W, BAR_H, bar_buf);

    /* Mark all dirty false after clear. */
    memset(dirty, 0, sizeof(dirty));
    memset(row_dirty, 0, sizeof(row_dirty));
    bar_dirty = false;

    printf("[VT100] Terminal initialized (80x30, 6x10 font, ANSI colour, status bar)\n");
}
