// preflash-window <socket path>
//
// The window onto a virtual panel, in a process of its own so that it stays where it is while
// the app restarts. It serves one app at a time over the socket, shows the frames that app
// writes into the shared frame buffer, turned the way the board is, with the board's buttons
// above and a status bar below, and sends back the mouse as touches and button presses. The
// only code outside the app that makes SDL video calls, all on this thread.

#include <SDL2/SDL.h>
#include <signal.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <vector>

#include "chrome.h"
#include "layout.h"
#include "link.h"
#include "pad.h"
#include "winlink/backlight.h"
#include "winlink/frame_buffer.h"

namespace {

using namespace preflash;
using nlohmann::json;
using window::Layout;
using window::Rect;

constexpr int kOrphanSeconds = 30;
constexpr int kBytesPerPixel = 2;
constexpr int kQuarter = 90;

class Window {
 public:
  ~Window() {
    if (texture_) SDL_DestroyTexture(texture_);
    painter_.reset();
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    if (started_) SDL_Quit();
  }

  bool open() const { return window_ != nullptr; }

  // Opens the window, or reshapes it to a new layout. An app that restarts sends the same
  // board again, and the window stays as it is. True in *blank when the panel's texture is new
  // and holds nothing yet.
  bool show(const Layout& layout, bool* blank) {
    *blank = false;
    if (!started_) {
      if (SDL_Init(SDL_INIT_VIDEO) != 0) return fail("couldn't start SDL");
      started_ = true;
      SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    }
    if (!window_) {
      window_ = SDL_CreateWindow(project_.c_str(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                 layout.window_w, layout.window_h, 0);
      if (!window_) return fail("couldn't open the window");
      renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_PRESENTVSYNC);
      if (!renderer_) return fail("couldn't draw to the window");
      painter_.emplace();
      if (!painter_->start(renderer_)) return fail("couldn't make the font");
    } else if (layout.window_w != window_w_ || layout.window_h != window_h_) {
      SDL_SetWindowSize(window_, layout.window_w, layout.window_h);
    }
    if (layout.window_w != window_w_ || layout.window_h != window_h_) {
      SDL_RenderSetLogicalSize(renderer_, layout.window_w, layout.window_h);
      window_w_ = layout.window_w;
      window_h_ = layout.window_h;
    }
    if (!texture_ || layout.panel.w != shown_w_ || layout.panel.h != shown_h_) {
      if (texture_) SDL_DestroyTexture(texture_);
      texture_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING,
                                   layout.panel.w, layout.panel.h);
      if (!texture_) return fail("couldn't make the panel's texture");
      shown_w_ = layout.panel.w;
      shown_h_ = layout.panel.h;
      *blank = true;
    }
    return true;
  }

  // The window's title is the project's name, and nothing else.
  void set_project(const std::string& project) {
    project_ = project;
    if (window_) SDL_SetWindowTitle(window_, project_.c_str());
  }
  void set_backlight(int percent) { backlight_ = percent; }
  int backlight() const { return backlight_; }

  // A picture already turned, the size of the panel as shown.
  void upload(const std::vector<uint8_t>& turned) {
    if (texture_) SDL_UpdateTexture(texture_, nullptr, turned.data(), shown_w_ * kBytesPerPixel);
  }

  void draw(const Layout& layout, const window::Chrome& chrome) {
    if (!window_) return;
    SDL_SetRenderDrawColor(renderer_, 0, 0, 0, 255);
    SDL_RenderClear(renderer_);
    painter_->strip(layout, chrome);
    const uint8_t level = winlink::backlight_level(backlight_);
    SDL_SetTextureColorMod(texture_, level, level, level);
    const SDL_Rect panel{layout.panel.x, layout.panel.y, layout.panel.w, layout.panel.h};
    SDL_RenderCopy(renderer_, texture_, nullptr, &panel);
    painter_->status(layout, chrome);
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
  std::optional<window::Painter> painter_;
  int window_w_ = 0;
  int window_h_ = 0;
  int shown_w_ = 0;
  int shown_h_ = 0;
  int backlight_ = 100;
  std::string project_ = "preflash";
};

int as_int(const json& m, const char* key, int fallback) {
  const auto it = m.find(key);
  return it != m.end() && it->is_number() ? it->get<int>() : fallback;
}

std::string as_text(const json& m, const char* key, const std::string& fallback) {
  const auto it = m.find(key);
  return it != m.end() && it->is_string() ? it->get<std::string>() : fallback;
}

// What a latched button's hover text adds to its wiring.
constexpr const char* kLatchedNote = " · latched";

struct BoardButton {
  std::string name;
  std::string wiring;
};

// Everything the app has said about the board, and what the window makes of it.
struct Device {
  std::string board;
  int panel_w = 0;
  int panel_h = 0;
  std::vector<BoardButton> buttons;
  int rotation = 0;
  std::string power = "on";
  std::vector<std::string> app_held;

  std::string status() const {
    std::string text = std::to_string(rotation) + "°";
    if (power == "off") text += " · off";
    if (power == "download") text += " · download mode";
    return text;
  }

  // The status bar's height follows the longest thing it can show at this rotation, so that
  // a hover or a power change doesn't resize the window.
  Layout layout() const {
    const int count = static_cast<int>(buttons.size());
    const int width = window::layout_for(panel_w, panel_h, rotation, count, 1).window_w;
    const std::string longest = std::to_string(rotation) + "° · download mode";
    int lines = window::status_lines_for(board, longest, width);
    for (const BoardButton& b : buttons) {
      lines = std::max(lines, window::status_lines_for(board, b.wiring + kLatchedNote, width));
    }
    return window::layout_for(panel_w, panel_h, rotation, count, lines);
  }
};

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

  Window win;
  Device device;
  Layout layout{};
  window::Pad pad;
  std::optional<winlink::FrameBuffer> frames;
  uint64_t seen_seq = 0;
  bool closing = false;
  bool failed = false;
  // What the mouse is doing: dragging on the panel, holding a board button, or holding rotate.
  bool mouse_held = false;
  std::string pressed;
  bool rotate_armed = false;
  int mouse_x = -1;
  int mouse_y = -1;
  bool mouse_in = false;
  // The last frame as the panel has it, and as the window shows it.
  std::vector<uint8_t> incoming, pixels, turned;
  bool redraw_frame = false;

  const auto send_button = [&](const std::string& name, bool down) {
    link.send({{"op", "button"}, {"name", name}, {"down", down}});
  };
  const auto let_go = [&](const std::string& name) {
    pad.release(name);
    send_button(name, false);
    if (pressed == name) pressed.clear();
  };
  const auto let_go_of_everything = [&]() {
    for (const std::string& name : pad.release_all()) send_button(name, false);
    pressed.clear();
  };
  const auto turn = [&](int by) { link.send({{"op", "rotate"}, {"by", by}}); };

  // A drag can leave the panel, and a release out there must still count, at its edge.
  const auto send_touch = [&](int x, int y, bool down) {
    if (device.panel_w <= 0) return;
    const Rect& p = layout.panel;
    const auto at = window::to_panel(layout, device.rotation, device.panel_w, device.panel_h,
                                     std::clamp(x, p.x, p.x + p.w - 1),
                                     std::clamp(y, p.y, p.y + p.h - 1));
    if (at) link.send({{"op", "touch"}, {"x", at->first}, {"y", at->second}, {"down", down}});
  };

  const auto button_at = [&](int x, int y) -> int {
    for (size_t i = 0; i < layout.buttons.size() && i < device.buttons.size(); ++i) {
      if (layout.buttons[i].contains(x, y)) return static_cast<int>(i);
    }
    return -1;
  };

  // Lays the window out again, as a new board, a turn or a new button list asks.
  const auto relayout = [&]() {
    if (device.panel_w <= 0 || device.panel_h <= 0) return;
    layout = device.layout();
    bool blank = false;
    if (!win.show(layout, &blank)) {
      failed = true;
      return;
    }
    if (blank) redraw_frame = true;
  };

  const auto on_message = [&](const json& m) {
    const std::string op = m["op"].get<std::string>();
    if (op == "board") {
      const int width = as_int(m, "width", 0);
      const int height = as_int(m, "height", 0);
      if (width <= 0 || height <= 0) return;
      const std::string project = as_text(m, "project", "");
      win.set_project(project.empty() ? "preflash" : project);
      device.board = as_text(m, "board", "");
      device.buttons.clear();
      std::vector<std::string> names;
      if (m.contains("buttons") && m["buttons"].is_array()) {
        for (const json& b : m["buttons"]) {
          if (!b.is_object() || !b.contains("name") || !b["name"].is_string()) continue;
          device.buttons.push_back({b["name"].get<std::string>(), as_text(b, "wiring", "")});
          names.push_back(device.buttons.back().name);
        }
      }
      // The app already has the held list from hello, and leaves out what its board lacks.
      pad.keep_only(names);
      if (std::find(names.begin(), names.end(), pressed) == names.end()) pressed.clear();
      if (width != device.panel_w || height != device.panel_h) {
        device.panel_w = width;
        device.panel_h = height;
        // The last frame was another panel's.
        pixels.clear();
        seen_seq = 0;
      }
      relayout();
      if (m.contains("frame_path") && m["frame_path"].is_string()) {
        // Nothing to show until the app has written a frame, so a failed open waits for the
        // next board and keeps the last one.
        if (auto buffer = winlink::FrameBuffer::open(m["frame_path"].get<std::string>())) {
          frames.emplace(std::move(*buffer));
          seen_seq = 0;
        }
      }
    } else if (op == "state") {
      win.set_backlight(std::clamp(as_int(m, "backlight", win.backlight()), 0, 100));
      device.power = as_text(m, "power", "on");
      device.app_held.clear();
      if (m.contains("held") && m["held"].is_array()) {
        for (const json& name : m["held"]) {
          if (name.is_string()) device.app_held.push_back(name.get<std::string>());
        }
      }
      const int rotation = window::normal_rotation(as_int(m, "rotation", device.rotation));
      if (rotation != device.rotation) {
        device.rotation = rotation;
        redraw_frame = true;
        relayout();
      }
    } else if (op == "close") {
      closing = true;
    }
  };

  while (!closing && !failed) {
    // Before anything the new app says, so that it hears this first whatever it sends.
    link.poll(on_message, [&] {
      // The new app never saw the press, so it gets nothing of this drag, not even its end.
      // The app that saw it lets go of it itself when the link drops. The mouse's buttons stay
      // held, and the new app hears which. Holds the app told of are its own to keep.
      mouse_held = false;
      link.send({{"op", "hello"}, {"held", pad.held()}});
    });

    if (win.open()) {
      SDL_Event e;
      while (SDL_PollEvent(&e)) {
        switch (e.type) {
          case SDL_WINDOWEVENT:
            if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST ||
                e.window.event == SDL_WINDOWEVENT_LEAVE) {
              let_go_of_everything();
              // A drag whose mouse-up never comes would leave the firmware touched.
              if (mouse_held) send_touch(mouse_x, mouse_y, false);
              mouse_held = false;
              // Nor may a missed mouse-up leave Rotate armed for a later one.
              rotate_armed = false;
              if (e.window.event == SDL_WINDOWEVENT_LEAVE) mouse_in = false;
              break;
            }
            if (e.window.event != SDL_WINDOWEVENT_CLOSE) break;
            [[fallthrough]];
          case SDL_QUIT:
            link.send({{"op", "quit"}});
            return 0;
          case SDL_MOUSEBUTTONDOWN:
            if (e.button.button != SDL_BUTTON_LEFT) break;
            mouse_x = e.button.x;
            mouse_y = e.button.y;
            mouse_in = true;
            if (const int i = button_at(mouse_x, mouse_y); i >= 0) {
              const std::string name = device.buttons[i].name;
              if (pad.latched(name)) {
                // Any click on a latched button lets it go.
                let_go(name);
              } else if (SDL_GetModState() & KMOD_ALT) {
                // ⌥-click holds it down with the mouse free, say BOOT while you click RST.
                pad.latch(name);
                send_button(name, true);
              } else {
                pressed = name;
                pad.press(pressed);
                send_button(pressed, true);
              }
            } else if (layout.rotate.contains(mouse_x, mouse_y)) {
              rotate_armed = true;
            } else if (layout.panel.contains(mouse_x, mouse_y)) {
              mouse_held = true;
              send_touch(mouse_x, mouse_y, true);
            }
            break;
          case SDL_MOUSEBUTTONUP:
            if (e.button.button != SDL_BUTTON_LEFT) break;
            mouse_x = e.button.x;
            mouse_y = e.button.y;
            if (!pressed.empty()) let_go(pressed);
            if (rotate_armed && layout.rotate.contains(mouse_x, mouse_y)) turn(kQuarter);
            rotate_armed = false;
            if (mouse_held) send_touch(mouse_x, mouse_y, false);
            mouse_held = false;
            break;
          case SDL_MOUSEMOTION:
            mouse_x = e.motion.x;
            mouse_y = e.motion.y;
            mouse_in = true;
            if (!pressed.empty()) {
              const int i = button_at(mouse_x, mouse_y);
              if (i < 0 || device.buttons[i].name != pressed) let_go(pressed);
            }
            if (mouse_held) send_touch(mouse_x, mouse_y, true);
            break;
          case SDL_KEYDOWN:
            if (e.key.repeat || !(e.key.keysym.mod & KMOD_GUI)) break;
            if (e.key.keysym.sym == SDLK_RIGHT) turn(kQuarter);
            if (e.key.keysym.sym == SDLK_LEFT) turn(-kQuarter);
            break;
          default:
            break;
        }
      }

      // Once the app is gone the last frame stays up, and dims or brightens no more.
      const size_t panel_bytes =
          static_cast<size_t>(device.panel_w) * device.panel_h * kBytesPerPixel;
      if (frames && frames->read_if_newer(&seen_seq, &incoming) && incoming.size() == panel_bytes) {
        pixels.swap(incoming);
        redraw_frame = true;
      }
      if (redraw_frame && pixels.size() == panel_bytes) {
        window::turn_rgb565(pixels.data(), device.panel_w, device.panel_h, device.rotation,
                            &turned);
        win.upload(turned);
      }
      redraw_frame = false;

      window::Chrome chrome;
      chrome.board = device.board;
      chrome.status = device.status();
      const int hovered = mouse_in ? button_at(mouse_x, mouse_y) : -1;
      const auto held = pad.held();
      for (size_t i = 0; i < device.buttons.size(); ++i) {
        const std::string& name = device.buttons[i].name;
        const bool down =
            std::find(held.begin(), held.end(), name) != held.end() ||
            std::find(device.app_held.begin(), device.app_held.end(), name) !=
                device.app_held.end();
        chrome.buttons.push_back({name, down, static_cast<int>(i) == hovered});
      }
      if (hovered >= 0 && !device.buttons[hovered].wiring.empty()) {
        chrome.status = device.buttons[hovered].wiring;
        if (pad.latched(device.buttons[hovered].name)) chrome.status += kLatchedNote;
        chrome.status_is_wiring = true;
      }
      chrome.rotate_hovered = mouse_in && layout.rotate.contains(mouse_x, mouse_y);
      chrome.rotate_down = rotate_armed && chrome.rotate_hovered;
      win.draw(layout, chrome);
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
