#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "esp_lcd_types.h"

// The virtual panel: what firmware has drawn, in the panel's own orientation and
// the board's size. Pixels are little-endian RGB565.
namespace preflash::panel {

// Panel coordinates, exclusive end. Pixels past the panel's edge are dropped. A caller
// that splits a frame into areas says which area ends it, so frames count once.
void draw(int x1, int y1, int x2, int y2, const void* rgb565, bool last_of_frame = true);

uint64_t frames_drawn();

// Called with the frame's number each time a frame finishes, on firmware's thread, once its
// pixels and the count have changed. It says only that a frame is ready. The callback must be
// quick and never wait for anything slow, and mustn't subscribe or unsubscribe. Returns a number
// for unsubscribe_frames.
using FrameSubscriber = std::function<void(uint64_t frame)>;
int subscribe_frames(FrameSubscriber subscriber);
void unsubscribe_frames(int subscription);

// Frames finished in the last second.
int fps();

// How far firmware has turned its picture on the panel, in degrees. LVGL's rotation counts,
// and firmware that draws through esp_lcd reads 0.
void set_rotation(int degrees);
int rotation();

// A percentage, clamped to 0-100.
void set_brightness(int pct);
int brightness();        // what the screen shows, after any backlight enable pin
int brightness_level();  // what firmware last set

// One line saying how the backlight stands, naming the enable pin if the board has one.
std::string backlight_status();

// The project, or the board if nothing names it, and what the screen shows.
std::string window_title();

// From now on, writes finished frames into the frame file at path, for the window process to
// show, at most one every few milliseconds from firmware's thread, with share_pending catching
// up a frame held back. A file a window still maps from an earlier run keeps its last frame until then.
// Throws std::runtime_error if it can't make the file.
void share_frames(const std::string& path);

// Writes a finished frame that was held back from the frame file. Call it on the main thread.
void share_pending();

// Stops writing the frame file, for tests.
void stop_sharing();

// Copies the whole panel. False, leaving out alone, if nothing changed since the last call.
bool take_frame(std::vector<uint8_t>* out);

// Copies the whole panel without touching what take_frame reports. If number isn't null it
// gets the frame the pixels belong to, as frames_drawn counted it, read under the same lock.
std::vector<uint8_t> copy_frame(uint64_t* number = nullptr);

// Writes the panel as a 24-bit BMP. Throws std::runtime_error if it can't.
void save_bmp(const char* path);

// The one handle firmware's esp_lcd calls take for this panel.
esp_lcd_panel_handle_t handle();
esp_lcd_panel_io_handle_t io_handle();

}  // namespace preflash::panel
