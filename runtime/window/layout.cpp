#include "layout.h"

#include <algorithm>

namespace preflash::window {
namespace {

bool sideways(int rotation) { return rotation == 90 || rotation == 270; }

// The first byte of a UTF-8 character, as opposed to one that continues it.
bool starts_glyph(char c) { return (static_cast<unsigned char>(c) & 0xC0) != 0x80; }

int glyphs_in(const std::string& text, size_t from, size_t to) {
  int count = 0;
  for (size_t i = from; i < to; ++i) count += starts_glyph(text[i]) ? 1 : 0;
  return count;
}

// Where `glyphs` characters after `from` end, stopping at `to`.
size_t after_glyphs(const std::string& text, size_t from, size_t to, int glyphs) {
  size_t at = from;
  for (int i = 0; i < glyphs && at < to; ++i) {
    ++at;
    while (at < to && !starts_glyph(text[at])) ++at;
  }
  return at;
}

}  // namespace

int normal_rotation(int rotation) {
  const int turned = ((rotation % 360) + 360) % 360;
  return turned % 90 == 0 ? turned : 0;
}

Layout layout_for(int panel_w, int panel_h, int rotation, int button_count, int status_lines) {
  rotation = normal_rotation(rotation);
  const int shown_w = sideways(rotation) ? panel_h : panel_w;
  const int shown_h = sideways(rotation) ? panel_w : panel_h;
  button_count = std::max(button_count, 0);
  status_lines = std::max(status_lines, 1);

  const int strip_w = 2 * kStripPadding + button_count * (kButtonWidth + kButtonGap) + kRotateWidth;
  Layout l{};
  l.window_w = std::max({kMinWindowWidth, shown_w, strip_w});
  const int status_h = 8 + kStatusLineHeight * status_lines;
  l.window_h = kStripHeight + shown_h + status_h;

  l.strip = {0, 0, l.window_w, kStripHeight};
  l.panel = {(l.window_w - shown_w) / 2, kStripHeight, shown_w, shown_h};
  l.status = {0, kStripHeight + shown_h, l.window_w, status_h};

  const int button_y = (kStripHeight - kButtonHeight) / 2;
  for (int i = 0; i < button_count; ++i) {
    l.buttons.push_back(
        {kStripPadding + i * (kButtonWidth + kButtonGap), button_y, kButtonWidth, kButtonHeight});
  }
  l.rotate = {l.window_w - kStripPadding - kRotateWidth, button_y, kRotateWidth, kButtonHeight};
  return l;
}

std::vector<Line> wrap_words(const std::string& text, int window_w) {
  const int per_line = std::max(1, (window_w - 2 * kStatusPadding) / kGlyph);
  std::vector<Line> lines;
  bool open = false;
  size_t line_start = 0;
  size_t line_end = 0;
  int used = 0;
  for (size_t i = 0; i < text.size();) {
    if (text[i] == ' ') {
      ++i;
      continue;
    }
    size_t end = text.find(' ', i);
    if (end == std::string::npos) end = text.size();
    const int glyphs = glyphs_in(text, i, end);
    // The line keeps every space between its words, each a byte and a glyph.
    const int gap = static_cast<int>(i - line_end);
    if (open && used + gap + glyphs <= per_line) {
      used += gap + glyphs;
      line_end = end;
    } else {
      if (open) lines.push_back({line_start, line_end - line_start});
      // A word wider than a line fills lines of its own until what's left fits.
      size_t from = i;
      while (glyphs_in(text, from, end) > per_line) {
        const size_t cut = after_glyphs(text, from, end, per_line);
        lines.push_back({from, cut - from});
        from = cut;
      }
      open = true;
      line_start = from;
      line_end = end;
      used = glyphs_in(text, from, end);
    }
    i = end;
  }
  if (open) lines.push_back({line_start, line_end - line_start});
  return lines;
}

std::string status_text(const std::string& board, const std::string& status) {
  if (status.empty()) return board;
  if (board.empty()) return status;
  return board + " " + status;
}

int status_lines_for(const std::string& board, const std::string& status, int window_w) {
  const auto lines = wrap_words(status_text(board, status), window_w);
  return std::max(1, static_cast<int>(lines.size()));
}

std::optional<std::pair<int, int>> to_panel(const Layout& layout, int rotation, int panel_w,
                                            int panel_h, int wx, int wy) {
  if (!layout.panel.contains(wx, wy)) return std::nullopt;
  const int sx = wx - layout.panel.x;
  const int sy = wy - layout.panel.y;
  switch (normal_rotation(rotation)) {
    case 90: return std::make_pair(sy, panel_h - 1 - sx);
    case 180: return std::make_pair(panel_w - 1 - sx, panel_h - 1 - sy);
    case 270: return std::make_pair(panel_w - 1 - sy, sx);
    default: return std::make_pair(sx, sy);
  }
}

void turn_rgb565(const uint8_t* in, int w, int h, int rotation, std::vector<uint8_t>* out) {
  rotation = normal_rotation(rotation);
  out->resize(static_cast<size_t>(w) * h * 2);
  const int out_w = sideways(rotation) ? h : w;
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      int ox = x;
      int oy = y;
      switch (rotation) {
        case 90: ox = h - 1 - y; oy = x; break;
        case 180: ox = w - 1 - x; oy = h - 1 - y; break;
        case 270: ox = y; oy = w - 1 - x; break;
        default: break;
      }
      const uint8_t* from = in + (static_cast<size_t>(y) * w + x) * 2;
      uint8_t* to = out->data() + (static_cast<size_t>(oy) * out_w + ox) * 2;
      to[0] = from[0];
      to[1] = from[1];
    }
  }
}

}  // namespace preflash::window
