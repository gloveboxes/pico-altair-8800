/* VT100 terminal emulator for ILI9488 display.
 *
 * 80×30 character terminal with ANSI colour support and software scrolling.
 * Bottom 20px reserved for a status bar showing IP address and CPU LEDs.
 * Escape sequence parsing uses a state machine that handles byte-at-a-time
 * input (sequences may arrive split across multiple calls).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Terminal dimensions */
#define VT_COLS 80
#define VT_ROWS 30

/* Initialise the VT100 terminal (clears screen, homes cursor). */
void vt100_init(void);

/* Process a single byte of terminal output through the VT100 state machine. */
void vt100_putchar(uint8_t c);

/* Non-blocking service — call from core 1 poll loop to flush dirty cells. */
void vt100_service(void);

/* Lightweight service — only renders the status bar if dirty. */
void vt100_service_status_bar(void);

/* Returns true when all pending drawing is complete. */
bool vt100_is_idle(void);

/* Update the status bar with CPU state (call from core 1 at ~20 Hz). */
void vt100_update_status(uint16_t address, uint8_t data, uint16_t status);

/* Update the IP address shown in the status bar (call once after WiFi connects). */
void vt100_set_ip(const char* ip);

#ifdef __cplusplus
}
#endif
