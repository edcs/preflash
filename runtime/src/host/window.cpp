#include "window.h"

#include <SDL2/SDL.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "preflash/board.h"
#include "preflash/panel.h"
#include "preflash/touch.h"

namespace preflash::window {
namespace {

constexpr int kBytesPerPixel = 2;
// Whatever else the host needs from SDL gets initialised with the window.
constexpr Uint32 kSdlSubsystems = SDL_INIT_VIDEO;

int g_width = 0;
int g_height = 0;
SDL_Renderer* g_renderer = nullptr;
SDL_Texture* g_texture = nullptr;

[[noreturn]] void fail(const char* what) {
  std::fprintf(stderr, "preflash: %s: %s\n", what, SDL_GetError());
  std::exit(1);
}

// A drag can leave the window, and a release out there must still count.
void touch_at(int x, int y, bool down) {
  touch::push({std::clamp(x, 0, g_width - 1), std::clamp(y, 0, g_height - 1), down});
}

void handle_events() {
  static bool held = false;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    switch (e.type) {
      case SDL_QUIT:
        // Not exit(): firmware threads may be holding locks that destructors would need.
        _exit(0);
      case SDL_MOUSEBUTTONDOWN:
      case SDL_MOUSEBUTTONUP:
        if (e.button.button == SDL_BUTTON_LEFT) {
          held = e.type == SDL_MOUSEBUTTONDOWN;
          touch_at(e.button.x, e.button.y, held);
        }
        break;
      case SDL_MOUSEMOTION:
        if (held) touch_at(e.motion.x, e.motion.y, true);
        break;
      default:
        break;
    }
  }
}

// Square-root response: 1% is dim but visible, like the panel in a dark room.
Uint8 backlight_level() {
  return static_cast<Uint8>(std::lround(255 * std::sqrt(panel::brightness() / 100.0)));
}

}  // namespace

void open() {
  g_width = board().width;
  g_height = board().height;
  if (SDL_Init(kSdlSubsystems) != 0) fail("couldn't start SDL");
  SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
  SDL_Window* window = SDL_CreateWindow(board().name.c_str(), SDL_WINDOWPOS_CENTERED,
                                        SDL_WINDOWPOS_CENTERED, g_width, g_height, 0);
  if (!window) fail("couldn't open the window");
  g_renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_PRESENTVSYNC);
  if (!g_renderer) fail("couldn't draw to the window");
  SDL_RenderSetLogicalSize(g_renderer, g_width, g_height);
  g_texture = SDL_CreateTexture(g_renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                g_width, g_height);
  if (!g_texture) fail("couldn't make the panel's texture");
}

void run() {
  std::vector<uint8_t> frame;
  for (;;) {
    handle_events();
    if (panel::take_frame(&frame)) {
      SDL_UpdateTexture(g_texture, nullptr, frame.data(), g_width * kBytesPerPixel);
    }
    const Uint8 level = backlight_level();
    SDL_SetTextureColorMod(g_texture, level, level, level);
    SDL_SetRenderDrawColor(g_renderer, 0, 0, 0, 255);
    SDL_RenderClear(g_renderer);
    SDL_RenderCopy(g_renderer, g_texture, nullptr, nullptr);
    SDL_RenderPresent(g_renderer);
    SDL_Delay(1);
  }
}

}  // namespace preflash::window
