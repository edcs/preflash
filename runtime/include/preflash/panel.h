#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "esp_lcd_types.h"

// The virtual panel: what firmware has drawn, in the panel's own orientation and
// the board's size. Pixels are little-endian RGB565.
namespace preflash::panel {

// Panel coordinates, exclusive end. Pixels past the panel's edge are dropped.
void draw(int x1, int y1, int x2, int y2, const void* rgb565);

// A percentage, clamped to 0-100.
void set_brightness(int pct);
int brightness();        // what the screen shows, after any backlight enable pin
int brightness_level();  // what firmware last set

// One line saying how the backlight stands, naming the enable pin if the board has one.
std::string backlight_status();

// Copies the whole panel. False, leaving out alone, if nothing changed since the last call.
bool take_frame(std::vector<uint8_t>* out);

// Writes the panel as a 24-bit BMP. Throws std::runtime_error if it can't.
void save_bmp(const char* path);

// The one handle firmware's esp_lcd calls take for this panel.
esp_lcd_panel_handle_t handle();
esp_lcd_panel_io_handle_t io_handle();

}  // namespace preflash::panel
