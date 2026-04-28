/* Shared SPI1 bus abstraction for Waveshare 3.5" display board.
 * Single owner of SPI1 peripheral init, shared data pins (SCK/MOSI/MISO),
 * and chip-select management for LCD (CS=9), touch (CS=16), and SD (CS=22).
 *
 * No other file should call spi_init(spi1, ...), gpio_set_function() on the
 * shared data pins, or gpio_init() on another device's CS pin.  Each device
 * driver only controls its own CS *after* acquiring the bus.
 *
 * Usage:
 *   spi1_bus_acquire_lcd();  // blocks, deselects all CS, sets LCD speed
 *   ... do SPI1 LCD transfers ...
 *   spi1_bus_release();      // releases bus for other users
 */
#pragma once

#include "pico/mutex.h"
#include "hardware/spi.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C"
{
#endif

// Initialize the shared SPI1 bus mutex (call once at startup)
void spi1_bus_init(void);

// Acquire exclusive access to SPI1 and set baudrate for LCD (62.5 MHz)
void spi1_bus_acquire_lcd(void);

// Acquire exclusive access to SPI1 and set baudrate for SD card (30 MHz)
void spi1_bus_acquire_sd(void);

// Hold SPI1 across a multi-call SD/FatFs operation.
void spi1_bus_begin_sd_session(void);

// Release a previously started SD/FatFs SPI1 session.
void spi1_bus_end_sd_session(void);

// Returns true when an SD/FatFs SPI1 session is active.
bool spi1_bus_sd_session_active(void);

// Release SPI1 bus
void spi1_bus_release(void);

#ifdef __cplusplus
}
#endif
