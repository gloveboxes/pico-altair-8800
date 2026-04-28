/* Shared SPI1 bus abstraction for Waveshare 3.5" display board.
 * Single owner of SPI1 peripheral init, shared data pins (SCK/MOSI/MISO),
 * and chip-select deselection for LCD (CS=9), touch (CS=16), and SD (CS=22).
 *
 * No other file should call spi_init(spi1, ...), gpio_set_function() on the
 * shared data pins, or gpio_init() on another device's CS pin.  Each device
 * driver only touches its own CS pin *after* acquiring the bus.
 */

#include "spi1_bus.h"
#include "hardware/gpio.h"
#include "hardware/spi.h"

#define SPI1_LCD_BAUDRATE  (62500000)  // 62.5 MHz for ILI9488
#define SPI1_SD_BAUDRATE   (25000000)  // 25 MHz — SD SPI standard speed

// Shared SPI1 pins
#define SPI1_SCK_PIN       10
#define SPI1_MOSI_PIN      11
#define SPI1_MISO_PIN      12

// Chip-select pins (directly managed here)
#define SPI1_LCD_CS_PIN    9
#define SPI1_TOUCH_CS_PIN  16
#define SPI1_SD_CS_PIN     22

static mutex_t g_spi1_mutex;
static bool g_sd_session_active = false;
static bool g_spi1_initialized = false;

// Deselect every device on the shared bus.
static inline void deselect_all(void)
{
    gpio_put(SPI1_LCD_CS_PIN, 1);
    gpio_put(SPI1_TOUCH_CS_PIN, 1);
    gpio_put(SPI1_SD_CS_PIN, 1);
}

// Drain stale bytes from the SPI1 RX FIFO and clear the overrun flag.
// The display DMA writes thousands of bytes but never reads the RX side,
// so the 8-entry PL022 FIFO fills and overflows.  If we don't drain it
// before an SD transaction, xchg_spi() reads a stale display byte instead
// of the real SD card response, corrupting the SD protocol.
static inline void drain_rx_fifo(void)
{
    while (spi_is_readable(spi1))
    {
        (void)spi_get_hw(spi1)->dr;
    }
    // Clear RX overrun (RORIC) and RX timeout (RTIC) interrupt flags
    spi_get_hw(spi1)->icr = 0x3;
}

static void spi1_bus_ensure_initialized(void)
{
    if (g_spi1_initialized)
    {
        return;
    }

    mutex_init(&g_spi1_mutex);

    // --- SPI1 peripheral and shared data pins (owned exclusively here) ---
    spi_init(spi1, SPI1_LCD_BAUDRATE);
    gpio_set_function(SPI1_SCK_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI1_MOSI_PIN, GPIO_FUNC_SPI);
    gpio_set_function(SPI1_MISO_PIN, GPIO_FUNC_SPI);

    // --- Chip-select pins for every device on the bus ---
    gpio_init(SPI1_LCD_CS_PIN);
    gpio_set_dir(SPI1_LCD_CS_PIN, GPIO_OUT);

    gpio_init(SPI1_TOUCH_CS_PIN);
    gpio_set_dir(SPI1_TOUCH_CS_PIN, GPIO_OUT);

    gpio_init(SPI1_SD_CS_PIN);
    gpio_set_dir(SPI1_SD_CS_PIN, GPIO_OUT);

    deselect_all();

    g_spi1_initialized = true;
}

void spi1_bus_init(void)
{
    spi1_bus_ensure_initialized();
}

void spi1_bus_acquire_lcd(void)
{
    spi1_bus_ensure_initialized();
    mutex_enter_blocking(&g_spi1_mutex);
    deselect_all();
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_baudrate(spi1, SPI1_LCD_BAUDRATE);
}

void spi1_bus_acquire_sd(void)
{
    spi1_bus_ensure_initialized();
    mutex_enter_blocking(&g_spi1_mutex);
    deselect_all();
    drain_rx_fifo();
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_baudrate(spi1, SPI1_SD_BAUDRATE);
}

void spi1_bus_begin_sd_session(void)
{
    spi1_bus_ensure_initialized();
    mutex_enter_blocking(&g_spi1_mutex);
    deselect_all();
    drain_rx_fifo();
    spi_set_format(spi1, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    spi_set_baudrate(spi1, SPI1_SD_BAUDRATE);
    g_sd_session_active = true;
}

void spi1_bus_end_sd_session(void)
{
    g_sd_session_active = false;
    mutex_exit(&g_spi1_mutex);
}

bool spi1_bus_sd_session_active(void)
{
    return g_sd_session_active;
}

void spi1_bus_release(void)
{
    mutex_exit(&g_spi1_mutex);
}
