/* Inky E-Ink Display Support for Altair 8800 Emulator
 * Displays system information on Pimoroni Pico Inky Pack
 */

#include "inky_display.h"

#ifdef INKY_SUPPORT

extern "C"
{
#include "build_version.h"
#include <stdio.h>
#include <string.h>
}

// C++ includes for Pimoroni libraries
#include "drivers/uc8151/uc8151.hpp"
#include "libraries/pico_graphics/pico_graphics.hpp"

// Pin definitions for Pico Inky Pack
enum InkyPin
{
    INKY_CS = 17,
    INKY_CLK = 18,
    INKY_MOSI = 19,
    INKY_DC = 20,
    INKY_RESET = 21,
    INKY_BUSY = 26,
};

// Static C++ objects (only created when INKY_SUPPORT is enabled)
static pimoroni::UC8151* g_uc8151 = nullptr;
static pimoroni::PicoGraphics_Pen1BitY* g_graphics = nullptr;

extern "C"
{

    void inky_display_init(void)
    {
        // Create the display and graphics objects
        // 296x128 pixel B&W e-ink display
        g_uc8151 = new pimoroni::UC8151(296, 128, pimoroni::ROTATE_0);
        g_graphics = new pimoroni::PicoGraphics_Pen1BitY(g_uc8151->width, g_uc8151->height, nullptr);

        // Clear display to white
        g_graphics->set_pen(0);
        g_graphics->clear();
    }

    void inky_display_update(const char* ssid, const char* ip)
    {
        if (!g_graphics || !g_uc8151)
        {
            return; // Not initialized
        }

        // Clear display to white
        g_graphics->set_pen(15);
        g_graphics->clear();

        // Set pen to black for text
        g_graphics->set_pen(0);

        int y_pos = 5;
        const int left_margin = 5;
        const int value_offset = 60; // Aligned offset for values
        char line_buffer[64];

        // Line 1: Title (larger font)
        g_graphics->set_font("bitmap14_outline");
        snprintf(line_buffer, sizeof(line_buffer), "ALTAIR 8800");
        g_graphics->text(line_buffer, {left_margin, y_pos}, 296);
        y_pos += 30;

        // Switch to bitmap8 font for remaining text
        g_graphics->set_font("bitmap8");

        // Line 2: Board name (label + value aligned)
        g_graphics->text("Board", {left_margin, y_pos}, 296);
        snprintf(line_buffer, sizeof(line_buffer), "%s", PICO_BOARD);
        g_graphics->text(line_buffer, {left_margin + value_offset, y_pos}, 296);
        y_pos += 18;

        // Line 3: Build version with date and time (label + value aligned)
        g_graphics->text("Build", {left_margin, y_pos}, 296);
        snprintf(line_buffer, sizeof(line_buffer), "v%d %s %s", BUILD_VERSION, BUILD_DATE, BUILD_TIME);
        g_graphics->text(line_buffer, {left_margin + value_offset, y_pos}, 296);
        y_pos += 24;

        // Line 4: WiFi SSID (label + value aligned)
        g_graphics->text("WiFi", {left_margin, y_pos}, 296);
        if (ssid && ssid[0] != '\0')
        {
            snprintf(line_buffer, sizeof(line_buffer), "%s", ssid);
        }
        else
        {
            snprintf(line_buffer, sizeof(line_buffer), "Not connected");
        }
        g_graphics->text(line_buffer, {left_margin + value_offset, y_pos}, 296);
        y_pos += 20;

        // Line 5: IP Address (label + value aligned)
        g_graphics->text("IP", {left_margin, y_pos}, 296);
        if (ip && ip[0] != '\0')
        {
            snprintf(line_buffer, sizeof(line_buffer), "%s", ip);
        }
        else
        {
            snprintf(line_buffer, sizeof(line_buffer), "---.---.---.---");
        }
        g_graphics->text(line_buffer, {left_margin + value_offset, y_pos}, 296);

        // Update the physical display
        g_uc8151->update(g_graphics);
    }

} // extern "C"

#endif // INKY_SUPPORT
