/* Pico Display 2.8" LCD Support for Altair 8800 Emulator
 * Displays system information on Pimoroni Pico Display 2.8"
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DISPLAY_2_8_SUPPORT

// Initialize the 2.8" LCD display
void display_2_8_init(void);

// Update the display with current system information
// Parameters:
//   ssid: WiFi SSID (NULL if not connected)
//   ip: IP address string (NULL if not connected)
void display_2_8_update(const char* ssid, const char* ip);

// Set the CPU status LED color
// Parameters:
//   cpu_running: true for green (running), false for red (stopped)
void display_2_8_set_cpu_led(bool cpu_running);

#else

// No-op stubs when Display 2.8 support is disabled
static inline void display_2_8_init(void) {}
static inline void display_2_8_update(const char* ssid, const char* ip) { (void)ssid; (void)ip; }
static inline void display_2_8_set_cpu_led(bool cpu_running) { (void)cpu_running; }

#endif // DISPLAY_2_8_SUPPORT

#ifdef __cplusplus
}
#endif
