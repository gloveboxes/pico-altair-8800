/* Waveshare Pico-ResTouch-LCD-3.5 — Shared SPI1 bus implementation. */

#include "ws_spi1_bus.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

/* ---- Baudrates ---- */
#define LCD_BAUDRATE  62500000   /* 62.5 MHz requested → 37.5 MHz actual */
#define SD_BAUDRATE   25000000   /* 25 MHz — SD SPI standard speed       */

/* ---- State ---- */
static mutex_t  g_mutex;
static bool     g_initialized      = false;
static bool     g_sd_session_active = false;

/* ---- Helpers ---- */

static inline void deselect_all(void)
{
    gpio_put(WS_PIN_LCD_CS, 1);
    gpio_put(WS_PIN_TOUCH_CS, 1);
    gpio_put(WS_PIN_SD_CS, 1);
}

/*
 * Drain stale bytes from the SPI1 RX FIFO and clear status flags.
 *
 * The display DMA writes thousands of TX bytes but never reads RX.
 * The PL022 FIFO (8 entries) overflows silently.  If we don't drain it
 * before an SD transaction, spi_write_read_blocking() returns a stale
 * display byte instead of the real SD response, corrupting the protocol.
 */
static inline void drain_rx_fifo(void)
{
    while (spi_is_readable(spi1))
        (void)spi_get_hw(spi1)->dr;

    spi_get_hw(spi1)->icr = 0x3;   /* Clear RORIC + RTIC */
}

/* ---- Public API ---- */

void ws_spi1_init(void)
{
    if (g_initialized)
        return;

    mutex_init(&g_mutex);

    /* SPI1 peripheral + shared data pins */
    spi_init(spi1, LCD_BAUDRATE);
    gpio_set_function(WS_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(WS_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_set_function(WS_PIN_MISO, GPIO_FUNC_SPI);

    /* Chip-select pins */
    gpio_init(WS_PIN_LCD_CS);
    gpio_set_dir(WS_PIN_LCD_CS, GPIO_OUT);
    gpio_init(WS_PIN_TOUCH_CS);
    gpio_set_dir(WS_PIN_TOUCH_CS, GPIO_OUT);
    gpio_init(WS_PIN_SD_CS);
    gpio_set_dir(WS_PIN_SD_CS, GPIO_OUT);

    deselect_all();
    g_initialized = true;
}

void ws_spi1_acquire_lcd(void)
{
    ws_spi1_init();
    mutex_enter_blocking(&g_mutex);
    deselect_all();
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_baudrate(spi1, LCD_BAUDRATE);
}

void ws_spi1_acquire_sd(void)
{
    ws_spi1_init();
    mutex_enter_blocking(&g_mutex);
    deselect_all();
    drain_rx_fifo();
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_baudrate(spi1, SD_BAUDRATE);
}

void ws_spi1_release(void)
{
    mutex_exit(&g_mutex);
}

void ws_spi1_begin_sd_session(void)
{
    ws_spi1_init();
    mutex_enter_blocking(&g_mutex);
    deselect_all();
    drain_rx_fifo();
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_baudrate(spi1, SD_BAUDRATE);
    g_sd_session_active = true;
}

void ws_spi1_end_sd_session(void)
{
    g_sd_session_active = false;
    mutex_exit(&g_mutex);
}

bool ws_spi1_sd_session_active(void)
{
    return g_sd_session_active;
}
