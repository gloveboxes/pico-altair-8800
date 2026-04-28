/* Waveshare Pico-ResTouch-LCD-3.5 — Shared SPI1 bus abstraction.
 *
 * Single owner of the SPI1 peripheral, shared data pins (SCK/MOSI/MISO),
 * and chip-select management for all three devices on the board:
 *   LCD  (ILI9488)  — CS GPIO 9
 *   Touch (XPT2046) — CS GPIO 16
 *   SD card          — CS GPIO 22
 *
 * No other file should call spi_init(spi1, …), gpio_set_function() on the
 * shared data pins, or gpio_init() on another device's CS pin.
 *
 * Every acquire helper:
 *   1. Takes the mutex (blocking)
 *   2. Deselects all three CS pins
 *   3. Drains the SPI RX FIFO (stale bytes from display DMA)
 *   4. Reconfigures format + baudrate for the target device
 */
#pragma once

#include "pico/mutex.h"
#include "hardware/spi.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Pin definitions (shared across all Waveshare drivers) ---- */
#define WS_PIN_LCD_CS   9
#define WS_PIN_SCK      10
#define WS_PIN_MOSI     11
#define WS_PIN_MISO     12
#define WS_PIN_TOUCH_CS 16
#define WS_PIN_SD_CS    22

/* ---- Lifecycle ---- */

/// Idempotent. Initialises SPI1 peripheral, data pins, all CS pins, mutex.
void ws_spi1_init(void);

/* ---- Per-device acquire / release ---- */

/// Acquire bus for LCD (37.5 MHz actual on RP2350 @ 150 MHz).
void ws_spi1_acquire_lcd(void);

/// Acquire bus for SD card (25 MHz — SD SPI standard speed).
void ws_spi1_acquire_sd(void);

/// Release the bus (any device).
void ws_spi1_release(void);

/* ---- SD session API (whole-FatFs-operation locking) ---- */

/// Acquire bus and mark an SD session active.
void ws_spi1_begin_sd_session(void);

/// End the SD session and release the bus.
void ws_spi1_end_sd_session(void);

/// Returns true while inside a begin/end SD session pair.
bool ws_spi1_sd_session_active(void);

#ifdef __cplusplus
}
#endif
