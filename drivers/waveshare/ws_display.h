/* Waveshare 3.5" ILI9488 — Altair 8800 front-panel compositor.
 * Displays status/address/data LEDs and WiFi info on 480×320.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef WAVESHARE_3_5_DISPLAY

void ws_display_init(void);
void ws_display_update_wifi(const char* ssid, const char* ip);
void ws_display_init_front_panel(void);
void ws_display_show_front_panel(uint16_t address, uint8_t data, uint16_t status);
void ws_display_poll(void);

#else

/* No-op stubs */
static inline void ws_display_init(void) {}
static inline void ws_display_update_wifi(const char* s, const char* i) { (void)s; (void)i; }
static inline void ws_display_init_front_panel(void) {}
static inline void ws_display_show_front_panel(uint16_t a, uint8_t d, uint16_t s) { (void)a; (void)d; (void)s; }
static inline void ws_display_poll(void) {}

#endif

#ifdef __cplusplus
}
#endif
