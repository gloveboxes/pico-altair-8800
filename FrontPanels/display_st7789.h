/* Pico Display 2.8" LCD Support for Altair 8800 Emulator
 * Displays system information on Pimoroni Pico Display 2.8"
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#ifdef DISPLAY_ST7789_SUPPORT

    // Initialize the 2.8" LCD display
    void display_st7789_init(void);

    // Update the display with current system information
    // Parameters:
    //   ssid: WiFi SSID (NULL if not connected)
    //   ip: IP address string (NULL if not connected)
    void display_st7789_update(const char* ssid, const char* ip);

    // Set the CPU status LED color
    // Parameters:
    //   cpu_running: true for green (running), false for red (stopped)
    void display_st7789_set_cpu_led(bool cpu_running);

    // Initialize the front panel LED display mode
    void display_st7789_init_front_panel(void);

    // Show the Altair front panel LEDs
    // Parameters:
    //   address: 16-bit address bus (A15-A0)
    //   data: 8-bit data bus (D7-D0)
    //   status: 16-bit status word (bits 0-9 for status LEDs)
    void display_st7789_show_front_panel(uint16_t address, uint8_t data, uint16_t status);

    // Get display statistics
    void display_st7789_get_stats(uint64_t* skipped_updates);

#else

// No-op stubs when Display 2.8 support is disabled
static inline void display_st7789_init(void) {}
static inline void display_st7789_update(const char* ssid, const char* ip)
{
    (void)ssid;
    (void)ip;
}
static inline void display_st7789_set_cpu_led(bool cpu_running)
{
    (void)cpu_running;
}
static inline void display_st7789_init_front_panel(void) {}
static inline void display_st7789_show_front_panel(uint16_t address, uint8_t data, uint16_t status)
{
    (void)address;
    (void)data;
    (void)status;
}

#endif // DISPLAY_ST7789_SUPPORT

#ifdef __cplusplus
}
#endif
