/* Waveshare Pico-ResTouch-LCD-3.5 — SD card low-level driver.
 *
 * This is a clean rewrite of the generic sdcard.c for the Waveshare board
 * where the SD card shares SPI1 with the ILI9488 display and XPT2046 touch.
 * All bus ownership goes through ws_spi1_bus; no #ifdef spaghetti.
 *
 * Implements the FatFs diskio interface (disk_initialize, disk_read, etc.)
 * so it is a drop-in replacement for the generic driver on this board.
 */

#include "ws_spi1_bus.h"
#include "hardware/spi.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "ff.h"
#include "diskio.h"

/* ---- SD/MMC command set ---- */
#define CMD0    0
#define CMD1    1
#define ACMD41  (0x80 + 41)
#define CMD8    8
#define CMD9    9
#define CMD12   12
#define ACMD13  (0x80 + 13)
#define CMD16   16
#define CMD17   17
#define CMD18   18
#define CMD24   24
#define CMD25   25
#define ACMD23  (0x80 + 23)
#define CMD32   32
#define CMD33   33
#define CMD38   38
#define CMD55   55
#define CMD58   58

/* Card type flags */
#define CT_MMC   0x01
#define CT_SD1   0x02
#define CT_SD2   0x04
#define CT_SDC   (CT_SD1 | CT_SD2)
#define CT_BLOCK 0x08

/* Clock rates */
#define CLK_SLOW (100 * 1000)       /* 100 kHz for card init */
#define CLK_FAST (25 * 1000 * 1000) /* 25 MHz — SD SPI standard speed */

#define SPI spi1

static volatile DSTATUS Stat = STA_NOINIT;
static BYTE CardType;

/* ---- Timing ---- */
static inline uint32_t millis(void) { return to_ms_since_boot(get_absolute_time()); }

/* ---- CS helpers ---- */
static inline void cs_low(void)  { asm volatile("nop\nnop\nnop"); gpio_put(WS_PIN_SD_CS, 0); asm volatile("nop\nnop\nnop"); }
static inline void cs_high(void) { asm volatile("nop\nnop\nnop"); gpio_put(WS_PIN_SD_CS, 1); asm volatile("nop\nnop\nnop"); }

/* ---- SPI byte exchange ---- */
static BYTE xchg(BYTE tx) {
    uint8_t b = tx;
    spi_write_read_blocking(SPI, &b, &b, 1);
    return b;
}

static void rcvr_multi(BYTE* buf, UINT n) {
    spi_read_blocking(SPI, 0xFF, buf, n);
}

static void xmit_multi(const BYTE* buf, UINT n) {
    spi_write_blocking(SPI, buf, n);
}

/* ---- Card helpers ---- */

static int wait_ready(UINT ms) {
    uint32_t t = millis();
    BYTE d;
    do { d = xchg(0xFF); } while (d != 0xFF && millis() < t + ms);
    return d == 0xFF;
}

static void deselect_card(void) {
    cs_high();
    xchg(0xFF);   /* dummy clock — force MISO hi-Z */
}

static int select_card(void) {
    cs_low();
    xchg(0xFF);
    if (wait_ready(500)) return 1;
    deselect_card();
    return 0;
}

static int rcvr_datablock(BYTE* buf, UINT len) {
    BYTE tok;
    uint32_t t = millis();
    do { tok = xchg(0xFF); } while (tok == 0xFF && millis() < t + 200);
    if (tok != 0xFE) return 0;
    rcvr_multi(buf, len);
    xchg(0xFF); xchg(0xFF);  /* discard CRC */
    return 1;
}

static int xmit_datablock(const BYTE* buf, BYTE token) {
    if (!wait_ready(500)) return 0;
    xchg(token);
    if (token != 0xFD) {
        xmit_multi(buf, 512);
        xchg(0xFF); xchg(0xFF);          /* dummy CRC */
        if ((xchg(0xFF) & 0x1F) != 0x05) return 0;
    }
    return 1;
}

static BYTE send_cmd(BYTE cmd, DWORD arg) {
    if (cmd & 0x80) {
        cmd &= 0x7F;
        BYTE r = send_cmd(CMD55, 0);
        if (r > 1) return r;
    }
    if (cmd != CMD12) { deselect_card(); if (!select_card()) return 0xFF; }
    xchg(0x40 | cmd);
    xchg((BYTE)(arg >> 24)); xchg((BYTE)(arg >> 16));
    xchg((BYTE)(arg >> 8));  xchg((BYTE)arg);
    BYTE crc = 0x01;
    if (cmd == CMD0) crc = 0x95;
    if (cmd == CMD8) crc = 0x87;
    xchg(crc);
    if (cmd == CMD12) xchg(0xFF);
    BYTE res; int n = 10;
    do { res = xchg(0xFF); } while ((res & 0x80) && --n);
    return res;
}

static int read_csd(BYTE* csd) {
    return (send_cmd(CMD9, 0) == 0) && rcvr_datablock(csd, 16);
}

/* ==== FatFs diskio interface ==== */

DSTATUS disk_initialize(BYTE drv)
{
    if (drv) return STA_NOINIT;

    bool owns = !ws_spi1_sd_session_active();
    if (owns) ws_spi1_acquire_sd();

    /* CS pin is already configured by ws_spi1_init — nothing else needed */
    sleep_ms(10);

    spi_set_baudrate(SPI, CLK_SLOW);
    cs_low();
    for (int i = 0; i < 10; i++) xchg(0xFF);  /* 80 dummy clocks */

    BYTE ty = 0;
    if (send_cmd(CMD0, 0) == 1) {
        uint32_t t = millis();
        if (send_cmd(CMD8, 0x1AA) == 1) {
            BYTE ocr[4];
            for (int i = 0; i < 4; i++) ocr[i] = xchg(0xFF);
            if (ocr[2] == 0x01 && ocr[3] == 0xAA) {
                while (millis() < t + 1000 && send_cmd(ACMD41, 1UL << 30)) ;
                if (millis() < t + 1000 && send_cmd(CMD58, 0) == 0) {
                    for (int i = 0; i < 4; i++) ocr[i] = xchg(0xFF);
                    ty = (ocr[0] & 0x40) ? CT_SD2 | CT_BLOCK : CT_SD2;
                }
            }
        } else {
            BYTE cmd_; 
            if (send_cmd(ACMD41, 0) <= 1) { ty = CT_SD1; cmd_ = ACMD41; }
            else                           { ty = CT_MMC; cmd_ = CMD1;   }
            while (millis() < t + 1000 && send_cmd(cmd_, 0)) ;
            if (millis() >= t + 1000 || send_cmd(CMD16, 512) != 0) ty = 0;
        }
    }
    CardType = ty;
    deselect_card();

    if (ty) { spi_set_baudrate(SPI, CLK_FAST); Stat &= ~STA_NOINIT; }
    else    { Stat = STA_NOINIT; }

    if (owns) ws_spi1_release();
    return Stat;
}

DSTATUS disk_status(BYTE drv) {
    return drv ? STA_NOINIT : Stat;
}

DRESULT disk_read(BYTE drv, BYTE* buf, LBA_t sector, UINT count)
{
    if (drv || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    bool owns = !ws_spi1_sd_session_active();
    if (owns) ws_spi1_acquire_sd();

    if (!(CardType & CT_BLOCK)) sector *= 512;
    if (count == 1) {
        if (send_cmd(CMD17, sector) == 0 && rcvr_datablock(buf, 512)) count = 0;
    } else {
        if (send_cmd(CMD18, sector) == 0) {
            do { if (!rcvr_datablock(buf, 512)) break; buf += 512; } while (--count);
            send_cmd(CMD12, 0);
        }
    }
    deselect_card();

    if (owns) ws_spi1_release();
    return count ? RES_ERROR : RES_OK;
}

#if !FF_FS_READONLY && !FF_FS_NORTC
DWORD get_fattime(void) { return 0; }
#endif

#if FF_FS_READONLY == 0
DRESULT disk_write(BYTE drv, const BYTE* buf, LBA_t sector, UINT count)
{
    if (drv || !count) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;
    if (Stat & STA_PROTECT) return RES_WRPRT;

    bool owns = !ws_spi1_sd_session_active();
    if (owns) ws_spi1_acquire_sd();

    if (!(CardType & CT_BLOCK)) sector *= 512;
    if (!select_card()) { if (owns) ws_spi1_release(); return RES_NOTRDY; }

    if (count == 1) {
        if (send_cmd(CMD24, sector) == 0 && xmit_datablock(buf, 0xFE)) count = 0;
    } else {
        if (CardType & CT_SDC) send_cmd(ACMD23, count);
        if (send_cmd(CMD25, sector) == 0) {
            do { if (!xmit_datablock(buf, 0xFC)) break; buf += 512; } while (--count);
            if (!xmit_datablock(0, 0xFD)) count = 1;
        }
    }
    deselect_card();

    if (owns) ws_spi1_release();
    return count ? RES_ERROR : RES_OK;
}
#endif

DRESULT disk_ioctl(BYTE drv, BYTE cmd, void* buf)
{
    if (drv) return RES_PARERR;
    if (Stat & STA_NOINIT) return RES_NOTRDY;

    bool owns = !ws_spi1_sd_session_active();
    if (owns) ws_spi1_acquire_sd();

    DRESULT res = RES_ERROR;
    BYTE csd[16];

    switch (cmd) {
    case CTRL_SYNC:
        if (select_card()) res = RES_OK;
        break;
    case GET_SECTOR_COUNT:
        if (read_csd(csd)) {
            DWORD cs;
            if ((csd[0] >> 6) == 1) {
                cs = csd[9] + ((WORD)csd[8] << 8) + ((DWORD)(csd[7] & 63) << 16) + 1;
                *(DWORD*)buf = cs << 10;
            } else {
                BYTE n = (csd[5] & 15) + ((csd[10] & 128) >> 7) + ((csd[9] & 3) << 1) + 2;
                cs = (csd[8] >> 6) + ((WORD)csd[7] << 2) + ((WORD)(csd[6] & 3) << 10) + 1;
                *(DWORD*)buf = cs << (n - 9);
            }
            res = RES_OK;
        }
        break;
    case GET_BLOCK_SIZE:
        if (CardType & CT_SD2) {
            if (send_cmd(ACMD13, 0) == 0) {
                xchg(0xFF);
                if (rcvr_datablock(csd, 16)) {
                    for (int i = 64 - 16; i; i--) xchg(0xFF);
                    *(DWORD*)buf = 16UL << (csd[10] >> 4);
                    res = RES_OK;
                }
            }
        } else if (read_csd(csd)) {
            if (CardType & CT_SD1)
                *(DWORD*)buf = (((csd[10] & 63) << 1) + ((WORD)(csd[11] & 128) >> 7) + 1) << ((csd[13] >> 6) - 1);
            else
                *(DWORD*)buf = ((WORD)((csd[10] & 124) >> 2) + 1) * (((csd[11] & 3) << 3) + ((csd[11] & 224) >> 5) + 1);
            res = RES_OK;
        }
        break;
    case CTRL_TRIM:
        if (!(CardType & CT_SDC)) break;
        if (!read_csd(csd)) break;
        if (!(csd[0] >> 6) && !(csd[10] & 0x40)) break;
        { DWORD *dp = buf, st = dp[0], ed = dp[1];
          if (!(CardType & CT_BLOCK)) { st *= 512; ed *= 512; }
          if (send_cmd(CMD32, st) == 0 && send_cmd(CMD33, ed) == 0
              && send_cmd(CMD38, 0) == 0 && wait_ready(30000))
              res = RES_OK;
        }
        break;
    default:
        res = RES_PARERR;
    }
    deselect_card();

    if (owns) ws_spi1_release();
    return res;
}
