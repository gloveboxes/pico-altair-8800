/* Inky E-Ink Display Support for Altair 8800 Emulator
 * Displays system information on Pimoroni Pico Inky Pack
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef INKY_SUPPORT

    // Initialize the Inky display
    void inky_display_init(void);

    // Update the display with current system information
    // Parameters:
    //   ssid: WiFi SSID (NULL if not connected)
    //   ip: IP address string (NULL if not connected)
    void inky_display_update(const char* ssid, const char* ip);

#else

// No-op stubs when Inky support is disabled
static inline void inky_display_init(void) {}
static inline void inky_display_update(const char* ssid, const char* ip)
{
    (void)ssid;
    (void)ip;
}

#endif // INKY_SUPPORT

#ifdef __cplusplus
}
#endif
