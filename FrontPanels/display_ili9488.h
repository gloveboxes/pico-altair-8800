/* Waveshare 3.5" ILI9488 LCD Support for Altair 8800 Emulator
 * Displays Altair front panel: status, address, data LEDs and WiFi info
 * 480x320 landscape - larger display allows generous LED spacing
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef WAVESHARE_3_5_DISPLAY

    // Initialize the ILI9488 display
    void display_ili9488_init(void);

    // Update the display with WiFi information
    // Parameters:
    //   ssid: WiFi SSID (NULL if not connected)
    //   ip: IP address string (NULL if not connected)
    void display_ili9488_update(const char* ssid, const char* ip);

    // Initialize the front panel LED display layout
    void display_ili9488_init_front_panel(void);

    // Show the Altair front panel LEDs
    // Parameters:
    //   address: 16-bit address bus (A15-A0)
    //   data: 8-bit data bus (D7-D0)
    //   status: 16-bit status word (bits 0-9 for status LEDs)
    void display_ili9488_show_front_panel(uint16_t address, uint8_t data, uint16_t status);

    // Service async DMA queue/completion from the main poll loop.
    void display_ili9488_poll(void);

    // Get display statistics
    void display_ili9488_get_stats(uint64_t* skipped_updates);

#else

// No-op stubs when Waveshare 3.5 display is not enabled
static inline void display_ili9488_init(void) {}
static inline void display_ili9488_update(const char* ssid, const char* ip)
{
    (void)ssid;
    (void)ip;
}
static inline void display_ili9488_init_front_panel(void) {}
static inline void display_ili9488_show_front_panel(uint16_t address, uint8_t data, uint16_t status)
{
    (void)address;
    (void)data;
    (void)status;
}
static inline void display_ili9488_poll(void) {}
static inline void display_ili9488_get_stats(uint64_t* skipped_updates)
{
    (void)skipped_updates;
}

#endif // WAVESHARE_3_5_DISPLAY

#ifdef __cplusplus
}
#endif
