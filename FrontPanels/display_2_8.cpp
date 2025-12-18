/* Pico Display 2.8" LCD Support for Altair 8800 Emulator
 * Displays system information on Pimoroni Pico Display 2.8"
 */

#include "display_2_8.h"

#ifdef DISPLAY_2_8_SUPPORT

extern "C" {
#include "build_version.h"
#include <stdio.h>
#include <string.h>
}

// C++ includes for Pimoroni libraries
#include "libraries/pico_display_28/pico_display_28.hpp"
#include "drivers/st7789/st7789.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"
#include "drivers/rgbled/rgbled.hpp"

// Static C++ objects (only created when DISPLAY_2_8_SUPPORT is enabled)
static pimoroni::ST7789* g_st7789 = nullptr;
static pimoroni::PicoGraphics_PenRGB332* g_graphics = nullptr;
static pimoroni::RGBLED* g_led = nullptr;

extern "C" {

void display_2_8_init(void)
{
    // Create the display and graphics objects
    // 320x240 pixel color LCD display (ST7789 driver)
    // Using BG_SPI_FRONT for the Pico Display 2.8
    g_st7789 = new pimoroni::ST7789(320, 240, pimoroni::ROTATE_0, false, pimoroni::get_spi_pins(pimoroni::BG_SPI_FRONT));
    g_graphics = new pimoroni::PicoGraphics_PenRGB332(g_st7789->width, g_st7789->height, nullptr);
    
    // Initialize RGB LED
    g_led = new pimoroni::RGBLED(pimoroni::PicoDisplay28::LED_R, 
                                  pimoroni::PicoDisplay28::LED_G, 
                                  pimoroni::PicoDisplay28::LED_B);
    g_led->set_rgb(77, 0, 0);  // Start with red at 30% brightness (CPU stopped)
    
    // Set backlight to full brightness
    g_st7789->set_backlight(255);
    
    // Clear display to black
    g_graphics->set_pen(0);
    g_graphics->clear();
    g_st7789->update(g_graphics);
}

void display_2_8_update(const char* ssid, const char* ip)
{
    if (!g_graphics || !g_st7789) {
        return;  // Not initialized
    }

    // Create color pens
    auto BG = g_graphics->create_pen(0, 0, 0);        // Black background
    auto TITLE = g_graphics->create_pen(0, 255, 255); // Cyan for title
    auto TEXT = g_graphics->create_pen(255, 255, 255); // White for text
    auto LABEL = g_graphics->create_pen(100, 200, 255); // Light blue for labels
    
    // Clear display to black
    g_graphics->set_pen(BG);
    g_graphics->clear();
    
    int y_pos = 10;
    const int left_margin = 10;
    char line_buffer[64];
    
    // Line 1: Title (larger font, cyan)
    g_graphics->set_pen(TITLE);
    g_graphics->set_font("bitmap14_outline");
    snprintf(line_buffer, sizeof(line_buffer), "ALTAIR 8800");
    g_graphics->text(line_buffer, {left_margin, y_pos}, 320, 3);  // Scale 3x
    y_pos += 50;
    
    // Switch to bitmap8 font for remaining text
    g_graphics->set_font("bitmap8");
    
    // Line 2: Board name (white)
    g_graphics->set_pen(TEXT);
    snprintf(line_buffer, sizeof(line_buffer), "Board: %s", PICO_BOARD);
    g_graphics->text(line_buffer, {left_margin, y_pos}, 320, 2);  // Scale 2x
    y_pos += 30;
    
    // Line 3: Build version with date and time (white)
    snprintf(line_buffer, sizeof(line_buffer), "Build: v%d %s %s", BUILD_VERSION, BUILD_DATE, BUILD_TIME);
    g_graphics->text(line_buffer, {left_margin, y_pos}, 320, 2);
    y_pos += 40;
    
    // Line 4: WiFi SSID (light blue label + white value)
    g_graphics->set_pen(LABEL);
    g_graphics->text("WiFi:", {left_margin, y_pos}, 320, 2);
    g_graphics->set_pen(TEXT);
    if (ssid && ssid[0] != '\0') {
        snprintf(line_buffer, sizeof(line_buffer), " %s", ssid);
    } else {
        snprintf(line_buffer, sizeof(line_buffer), " Not connected");
    }
    g_graphics->text(line_buffer, {left_margin + 60, y_pos}, 320, 2);
    y_pos += 30;
    
    // Line 5: IP Address (light blue label + white value)
    g_graphics->set_pen(LABEL);
    g_graphics->text("IP:", {left_margin, y_pos}, 320, 2);
    g_graphics->set_pen(TEXT);
    if (ip && ip[0] != '\0') {
        snprintf(line_buffer, sizeof(line_buffer), "  %s", ip);
    } else {
        snprintf(line_buffer, sizeof(line_buffer), "  ---.---.---.---");
    }
    g_graphics->text(line_buffer, {left_margin + 54, y_pos}, 320, 2);
    
    // Update the physical display
    g_st7789->update(g_graphics);
}

void display_2_8_set_cpu_led(bool cpu_running)
{
    if (!g_led) {
        return;  // Not initialized
    }
    
    if (cpu_running) {
        g_led->set_rgb(0, 77, 0);  // Green for running (30% brightness)
    } else {
        g_led->set_rgb(77, 0, 0);  // Red for stopped (30% brightness)
    }
}

} // extern "C"

#endif // DISPLAY_2_8_SUPPORT
