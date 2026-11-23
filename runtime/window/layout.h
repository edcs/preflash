#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Where everything sits in the device window, as pure geometry so that tests can reach it
// without SDL. From the top: the button strip, the panel turned by the board's rotation and
// centred on black, and the status bar. Sizes are in window points.
namespace preflash::window {

constexpr int kMinWindowWidth = 250;
constexpr int kStripHeight = 40;
constexpr int kStripPadding = 10;
constexpr int kButtonHeight = 28;
constexpr int kButtonGap = 6;
// Wide enough for a four-letter label, as BOOT, with room either side.
constexpr int kButtonWidth = 52;
constexpr int kRotateWidth = 44;
constexpr int kStatusLineHeight = 16;
constexpr int kStatusPadding = 14;
constexpr int kGlyph = 8;

struct Rect {
  int x, y, w, h;
  bool contains(int px, int py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};

struct Layout {
  int window_w, window_h;
  Rect strip, panel, status;
  std::vector<Rect> buttons;
  Rect rotate;
};

// The window for a panel_w x panel_h panel turned clockwise by `rotation`, with
// `button_count` board buttons and a status bar of `status_lines` lines.
Layout layout_for(int panel_w, int panel_h, int rotation, int button_count, int status_lines);

// One line of wrapped text: `length` bytes from `start`, both in bytes of the text given.
struct Line {
  size_t start;
  size_t length;
};

// Whole words that fit a status bar `window_w` wide, a glyph to each UTF-8 character. A word
// wider than a line is broken where the line ends.
std::vector<Line> wrap_words(const std::string& text, int window_w);

// What the status bar shows, as one text: the board's name, then the status.
std::string status_text(const std::string& board, const std::string& status);

// How many lines the board's name and the status take in a window `window_w` wide.
int status_lines_for(const std::string& board, const std::string& status, int window_w);

// The panel pixel under window point (wx, wy), or nullopt off the panel.
std::optional<std::pair<int, int>> to_panel(const Layout& layout, int rotation, int panel_w,
                                            int panel_h, int wx, int wy);

// The w x h RGB565 picture `in`, turned clockwise by `rotation`, into *out. A quarter turn
// either way gives an h x w picture.
void turn_rgb565(const uint8_t* in, int w, int h, int rotation, std::vector<uint8_t>* out);

// 90, 180 or 270 for those angles and any that differ from them by whole turns, and 0 for
// anything else.
int normal_rotation(int rotation);

}  // namespace preflash::window
