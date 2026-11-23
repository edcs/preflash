#include "chrome.h"

#include <algorithm>
#include <cstdint>
#include <vector>

#include "font8x8.h"

namespace preflash::window {
namespace {

// The mockup's colours.
constexpr uint32_t kStripBack = 0x1B2024;
constexpr uint32_t kRule = 0x2A3036;
constexpr uint32_t kKeyBack = 0x24292E;
constexpr uint32_t kKeyEdge = 0x3A4249;
constexpr uint32_t kText = 0xE7ECEF;
constexpr uint32_t kDim = 0x8D98A1;
constexpr uint32_t kAccent = 0x5FD4E8;
constexpr uint32_t kOnAccent = 0x0E1113;
constexpr uint32_t kStatusBack = 0x15191C;

// font8x8_basic covers ASCII. The status needs two more, drawn here in its style.
constexpr unsigned char kDegree[8] = {0x1C, 0x36, 0x36, 0x1C, 0x00, 0x00, 0x00, 0x00};
constexpr unsigned char kMiddleDot[8] = {0x00, 0x00, 0x00, 0x18, 0x18, 0x00, 0x00, 0x00};
constexpr int kDegreeGlyph = 128;
constexpr int kMiddleDotGlyph = 129;
constexpr int kGlyphCount = 130;
constexpr int kIcon = 16;

const unsigned char* glyph_rows(int glyph) {
  if (glyph == kDegreeGlyph) return kDegree;
  if (glyph == kMiddleDotGlyph) return kMiddleDot;
  return font8x8_basic[glyph];
}

// The glyph for the UTF-8 character at s[*at], moving *at past it. Anything the font lacks
// shows as a question mark.
int next_glyph(const std::string& s, size_t* at) {
  const auto lead = static_cast<unsigned char>(s[*at]);
  size_t length = 1;
  if (lead >= 0xF0) {
    length = 4;
  } else if (lead >= 0xE0) {
    length = 3;
  } else if (lead >= 0xC0) {
    length = 2;
  }
  const auto second = *at + 1 < s.size() ? static_cast<unsigned char>(s[*at + 1]) : 0;
  int glyph = '?';
  if (lead < 0x80) {
    glyph = lead;
  } else if (lead == 0xC2 && second == 0xB0) {
    glyph = kDegreeGlyph;
  } else if (lead == 0xC2 && second == 0xB7) {
    glyph = kMiddleDotGlyph;
  }
  *at = std::min(*at + length, s.size());
  return glyph;
}

int glyph_count(const std::string& s) {
  int count = 0;
  for (size_t at = 0; at < s.size(); ++count) next_glyph(s, &at);
  return count;
}

}  // namespace

Painter::~Painter() {
  if (font_) SDL_DestroyTexture(font_);
}

bool Painter::start(SDL_Renderer* renderer) {
  renderer_ = renderer;
  if (font_) SDL_DestroyTexture(font_);
  font_ = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC,
                            kGlyphCount * kGlyph, kGlyph);
  if (!font_) return false;
  std::vector<uint32_t> pixels(static_cast<size_t>(kGlyphCount) * kGlyph * kGlyph, 0);
  for (int glyph = 0; glyph < kGlyphCount; ++glyph) {
    const unsigned char* rows = glyph_rows(glyph);
    for (int y = 0; y < kGlyph; ++y) {
      for (int x = 0; x < kGlyph; ++x) {
        if (rows[y] & (1u << x)) pixels[y * kGlyphCount * kGlyph + glyph * kGlyph + x] = 0xFFFFFFFF;
      }
    }
  }
  if (SDL_UpdateTexture(font_, nullptr, pixels.data(), kGlyphCount * kGlyph * 4) != 0) return false;
  SDL_SetTextureBlendMode(font_, SDL_BLENDMODE_BLEND);
  return true;
}

void Painter::fill(const Rect& r, uint32_t rgb) {
  SDL_SetRenderDrawColor(renderer_, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
  const SDL_Rect rect{r.x, r.y, r.w, r.h};
  SDL_RenderFillRect(renderer_, &rect);
}

void Painter::outline(const Rect& r, uint32_t rgb) {
  SDL_SetRenderDrawColor(renderer_, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
  const SDL_Rect rect{r.x, r.y, r.w, r.h};
  SDL_RenderDrawRect(renderer_, &rect);
}

int Painter::text(int x, int y, const std::string& s, uint32_t rgb) {
  SDL_SetTextureColorMod(font_, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF);
  for (size_t at = 0; at < s.size(); x += kGlyph) {
    const int glyph = next_glyph(s, &at);
    const SDL_Rect from{glyph * kGlyph, 0, kGlyph, kGlyph};
    const SDL_Rect to{x, y, kGlyph, kGlyph};
    SDL_RenderCopy(renderer_, font_, &from, &to);
  }
  return x;
}

// After the mockup's: a small square, and an arrow curling over it clockwise.
void Painter::rotate_icon(const Rect& r, uint32_t rgb) {
  const int ox = r.x + (r.w - kIcon) / 2;
  const int oy = r.y + (r.h - kIcon) / 2;
  SDL_SetRenderDrawColor(renderer_, rgb >> 16, (rgb >> 8) & 0xFF, rgb & 0xFF, 255);
  const SDL_Rect box{ox + 2, oy + 6, 8, 8};
  SDL_RenderDrawRect(renderer_, &box);
  const SDL_Point arc[] = {{ox + 7, oy + 2}, {ox + 9, oy + 2},  {ox + 11, oy + 3},
                           {ox + 12, oy + 4}, {ox + 13, oy + 6}, {ox + 13, oy + 9}};
  SDL_RenderDrawLines(renderer_, arc, sizeof arc / sizeof arc[0]);
  const SDL_Point head[] = {{ox + 11, oy + 7}, {ox + 13, oy + 9}, {ox + 15, oy + 7}};
  SDL_RenderDrawLines(renderer_, head, sizeof head / sizeof head[0]);
}

void Painter::strip(const Layout& layout, const Chrome& chrome) {
  fill(layout.strip, kStripBack);
  fill({0, layout.strip.y + layout.strip.h - 1, layout.strip.w, 1}, kRule);

  const auto key = [this](const Rect& r, bool down, bool hovered) {
    fill(r, down ? kAccent : kKeyBack);
    outline(r, down || hovered ? kAccent : kKeyEdge);
    return down ? kOnAccent : kText;
  };
  for (size_t i = 0; i < layout.buttons.size() && i < chrome.buttons.size(); ++i) {
    const Rect& r = layout.buttons[i];
    const Chrome::Button& b = chrome.buttons[i];
    const uint32_t ink = key(r, b.down, b.hovered);
    const int w = glyph_count(b.label) * kGlyph;
    text(r.x + (r.w - w) / 2, r.y + (r.h - kGlyph) / 2, b.label, ink);
  }
  rotate_icon(layout.rotate, key(layout.rotate, chrome.rotate_down, chrome.rotate_hovered));
}

void Painter::status(const Layout& layout, const Chrome& chrome) {
  fill(layout.status, kStatusBack);
  fill({0, layout.status.y, layout.status.w, 1}, kRule);

  // The board's name is brighter than what follows it, wrapped as one text.
  const std::string all = status_text(chrome.board, chrome.status);
  const size_t board_end = chrome.board.size();
  const uint32_t after = chrome.status_is_wiring ? kAccent : kDim;
  int y = layout.status.y + 4 + (kStatusLineHeight - kGlyph) / 2;
  for (const Line& line : wrap_words(all, layout.window_w)) {
    const size_t end = line.start + line.length;
    int x = kStatusPadding;
    if (line.start < board_end) {
      x = text(x, y, all.substr(line.start, std::min(end, board_end) - line.start), kText);
    }
    if (end > board_end) {
      const size_t from = std::max(line.start, board_end);
      text(x, y, all.substr(from, end - from), after);
    }
    y += kStatusLineHeight;
  }
}

}  // namespace preflash::window
