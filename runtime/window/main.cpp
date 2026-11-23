// preflash-window <socket path>
//
// The window onto a virtual panel, in a process of its own so that it stays where it is while
// the app restarts. It serves one app at a time over the socket, shows the frames that app
// writes into the shared frame buffer, and sends back the mouse as touches. The only code
// outside the app that makes SDL video calls, all on this thread.

#include <SDL2/SDL.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "link.h"
#include "winlink/backlight.h"
#include "winlink/frame_buffer.h"

namespace {

using namespace preflash;
using nlohmann::json;

constexpr int kOrphanSeconds = 30;
constexpr int kBytesPerPixel = 2;

class Window {
 public:
  ~Window() {
    if (texture_) SDL_DestroyTexture(texture_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    if (started_) SDL_Quit();
  }

  bool open() const { return window_ != nullptr; }

  // Opens the window, or reshapes it if the panel's size has changed. An app that restarts
  // sends the same board again, and the window stays as it is.
  bool show(int width, int height) {
    if (!started_) {
      if (SDL_Init(SDL_INIT_VIDEO) != 0) return fail("couldn't start SDL");
      started_ = true;
      SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    }
    if (window_ && width == width_ && height == height_) return true;
    if (!window_) {
      window_ = SDL_CreateWindow(title_.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 width, height, 0);
      if (!window_) return fail("couldn't open the window");
      renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_PRESENTVSYNC);
      if (!renderer_) return fail("couldn't draw to the window");
    } else {
      SDL_SetWindowSize(window_, width, height);
    }
    if (texture_) SDL_DestroyTexture(texture_);
    texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                 width, height);
    if (!texture_) return fail("couldn't make the panel's texture");
    SDL_RenderSetLogicalSize(renderer_, width, height);
    width_ = width;
    height_ = height;
    return true;
  }

  void set_title(const std::string& project, int backlight) {
    project_ = project;
    backlight_ = backlight;
    title_ = project_ + " - backlight " + std::to_string(backlight_) + "%";
    if (window_) SDL_SetWindowTitle(window_, title_.c_str());
  }
  void set_backlight(int percent) { set_title(project_, percent); }
  int backlight() const { return backlight_; }
  int width() const { return width_; }
  int height() const { return height_; }

  void draw(const std::vector<uint8_t>* frame) {
    if (!window_) return;
    if (frame) SDL_UpdateTexture(texture_, nullptr, frame->data(), width_ * kBytesPerPixel);
    const uint8_t level = winlink::backlight_level(backlight_);
    SDL_SetTextureColorMod(texture_, level, level, level);
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    SDL_RenderCopy(renderer_, texture_, nullptr, nullptr);
    SDL_RenderPresent(renderer_);
  }

 private:
  bool fail(const char* what) {
    std::fprintf(stderr, "preflash-window: %s: %s\n", what, SDL_GetError());
    return false;
  }

  bool started_ = false;
  SDL_Window* window_ = nullptr;
  SDL_Renderer* renderer_ = nullptr;
  SDL_Texture* texture_ = nullptr;
  int width_ = 0;
  int height_ = 0;
  int backlight_ = 100;
  std::string project_ = "preflash";
  std::string title_ = "preflash";
};

int as_int(const json& m, const char* key, int fallback) {
  const auto it = m.find(key);
  return it != m.end() && it->is_number() ? it->get<int>() : fallback;
}

// A drag can leave the window, and a release out there must still count.
void send_touch(window::Link& link, const Window& w, int x, int y, bool down) {
  if (w.width() <= 0) return;
  link.send({{"op", "touch"},
             {"x", std::clamp(x, 0, w.width() - 1)},
             {"y", std::clamp(y, 0, w.height() - 1)},
             {"down", down}});
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: preflash-window <socket path>\n");
    return 2;
  }
  // A dead app mustn't kill the window on its way out.
  signal(SIGPIPE, SIG_IGN);

  window::Link link;
  std::string error;
  if (!link.listen(argv[1], &error)) {
    std::fprintf(stderr, "preflash-window: %s\n", error.c_str());
    return 1;
  }

  Window window;
  std::optional<winlink::FrameBuffer> frames;
  uint64_t seen_seq = 0;
  bool closing = false;
  bool failed = false;
  uint64_t greeted = 0;
  bool mouse_held = false;

  const auto on_message = [&](const json& m) {
    const std::string op = m["op"].get<std::string>();
    if (op == "board") {
      const int width = as_int(m, "width", 0);
      const int height = as_int(m, "height", 0);
      const std::string project =
          m.contains("project") && m["project"].is_string() ? m["project"].get<std::string>() : "";
      if (width <= 0 || height <= 0) return;
      window.set_title(project.empty() ? "preflash" : project, window.backlight());
      if (!window.show(width, height)) {
        failed = true;
        return;
      }
      if (m.contains("frame_path") && m["frame_path"].is_string()) {
        // Nothing to show until the app has written a frame, so a failed open waits for the
        // next board and keeps the last one.
        if (auto buffer = winlink::FrameBuffer::open(m["frame_path"].get<std::string>())) {
          frames.emplace(std::move(*buffer));
          seen_seq = 0;
        }
      }
    } else if (op == "state") {
      window.set_backlight(std::clamp(as_int(m, "backlight", window.backlight()), 0, 100));
    } else if (op == "close") {
      closing = true;
    }
  };

  std::vector<uint8_t> pixels;
  while (!closing && !failed) {
    link.poll(on_message);
    if (link.connections() != greeted) {
      greeted = link.connections();
      mouse_held = false;
      link.send({{"op", "hello"}, {"held", json::array()}});
    }

    if (window.open()) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        switch (e.type) {
          case SDL_WINDOWEVENT:
            if (e.window.event != SDL_WINDOWEVENT_CLOSE) break;
            [[fallthrough]];
          case SDL_QUIT:
            link.send({{"op", "quit"}});
            return 0;
          case SDL_MOUSEBUTTONDOWN:
          case SDL_MOUSEBUTTONUP:
            if (e.button.button == SDL_BUTTON_LEFT) {
              mouse_held = e.type == SDL_MOUSEBUTTONDOWN;
              send_touch(link, window, e.button.x, e.button.y, mouse_held);
            }
            break;
          case SDL_MOUSEMOTION:
            if (mouse_held) send_touch(link, window, e.motion.x, e.motion.y, true);
            break;
          default:
            break;
        }
      }
      // Once the app is gone the last frame stays up, and dims or brightens no more.
      const bool fresh = frames && frames->read_if_newer(&seen_seq, &pixels) &&
                         pixels.size() ==
                             static_cast<size_t>(window.width()) * window.height() * kBytesPerPixel;
      window.draw(fresh ? &pixels : nullptr);
      SDL_Delay(1);
    } else {
      SDL_Delay(5);
    }

    if (!link.connected() &&
        std::chrono::steady_clock::now() - link.last_connected() >
            std::chrono::seconds(kOrphanSeconds)) {
      break;
    }
  }
  return failed ? 1 : 0;
}
