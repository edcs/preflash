#pragma once

#include <SDL2/SDL.h>

#include <string>
#include <vector>

#include "layout.h"

// The window's own parts around the panel: the button strip above it and the status bar below,
// drawn upright whichever way the panel is turned, in the built-in 8x8 font.
namespace preflash::window {

struct Chrome {
  struct Button {
    std::string label;
    bool down = false;
    bool hovered = false;
  };
  std::vector<Button> buttons;  // in the order of the layout's
  bool rotate_hovered = false;
  bool rotate_down = false;
  std::string board;
  // The status, or a hovered button's wiring in its place.
  std::string status;
  bool status_is_wiring = false;
};

class Painter {
 public:
  Painter() = default;
  ~Painter();
  Painter(const Painter&) = delete;
  Painter& operator=(const Painter&) = delete;

  // Makes the font's texture for this renderer. False, with SDL's error set, if it can't.
  bool start(SDL_Renderer* renderer);
  void strip(const Layout& layout, const Chrome& chrome);
  void status(const Layout& layout, const Chrome& chrome);

 private:
  void fill(const Rect& r, uint32_t rgb);
  void outline(const Rect& r, uint32_t rgb);
  // UTF-8 text on one line from its top left. Returns where the next glyph would go.
  int text(int x, int y, const std::string& s, uint32_t rgb);
  void rotate_icon(const Rect& r, uint32_t rgb);

  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* font_ = nullptr;
};

}  // namespace preflash::window
