#include "preflash/panel.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>

#include "preflash/board.h"
#include "preflash/bus.h"
#include "preflash/touch.h"

namespace preflash::panel {
namespace {

constexpr int kBytesPerPixel = 2;
constexpr int kBmpHeaderSize = 54;
constexpr int kBmpBytesPerPixel = 3;

std::atomic<int> g_brightness{100};

struct Pixels {
  std::mutex mutex;
  int width = board().width;
  int height = board().height;
  std::vector<uint8_t> data = std::vector<uint8_t>(width * height * kBytesPerPixel);
  bool changed = true;
};

Pixels& pixels() {
  static Pixels p;
  return p;
}

void put_le(std::vector<char>* out, size_t at, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) (*out)[at + i] = static_cast<char>(value >> (8 * i));
}

}  // namespace

void draw(int x1, int y1, int x2, int y2, const void* rgb565) {
  Pixels& p = pixels();
  const int source_stride = (x2 - x1) * kBytesPerPixel;
  const int left = std::max(x1, 0);
  const int right = std::min(x2, p.width);
  const int top = std::max(y1, 0);
  const int bottom = std::min(y2, p.height);
  if (left >= right || top >= bottom) return;

  const auto* source = static_cast<const uint8_t*>(rgb565);
  std::lock_guard<std::mutex> lock(p.mutex);
  for (int y = top; y < bottom; ++y) {
    const uint8_t* from = source + (y - y1) * source_stride + (left - x1) * kBytesPerPixel;
    uint8_t* to = p.data.data() + (y * p.width + left) * kBytesPerPixel;
    std::memcpy(to, from, (right - left) * kBytesPerPixel);
  }
  p.changed = true;
}

void set_brightness(int pct) { g_brightness = std::clamp(pct, 0, 100); }

// A separate enable pin cuts the driver's power, so it wins over any PWM level.
int brightness() {
  const std::optional<int>& enable = board().backlight.enable_pin;
  if (enable && !gpio_read(*enable)) return 0;
  return g_brightness;
}

int brightness_level() { return g_brightness; }

std::string backlight_status() {
  std::string status =
      "brightness: " + std::to_string(brightness()) + " (pwm " + std::to_string(g_brightness);
  const std::optional<int>& enable = board().backlight.enable_pin;
  if (enable) {
    status += ", enable gpio" + std::to_string(*enable) + (gpio_read(*enable) ? " high" : " low");
  }
  return status + ")";
}

std::string window_title() {
  const char* project = std::getenv("PREFLASH_PROJECT");
  return (project ? std::string(project) : board().name) + " - backlight " +
         std::to_string(brightness()) + "%";
}

bool take_frame(std::vector<uint8_t>* out) {
  Pixels& p = pixels();
  std::lock_guard<std::mutex> lock(p.mutex);
  if (!p.changed) return false;
  *out = p.data;
  p.changed = false;
  return true;
}

void save_bmp(const char* path) {
  Pixels& p = pixels();
  std::vector<uint8_t> frame;
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    frame = p.data;
  }

  const int row_size = (p.width * kBmpBytesPerPixel + 3) & ~3;
  const size_t image_size = static_cast<size_t>(row_size) * p.height;
  std::vector<char> bmp(kBmpHeaderSize + image_size);
  bmp[0] = 'B';
  bmp[1] = 'M';
  put_le(&bmp, 2, static_cast<uint32_t>(bmp.size()), 4);
  put_le(&bmp, 10, kBmpHeaderSize, 4);
  put_le(&bmp, 14, 40, 4);  // size of the info header
  put_le(&bmp, 18, p.width, 4);
  put_le(&bmp, 22, p.height, 4);
  put_le(&bmp, 26, 1, 2);  // planes
  put_le(&bmp, 28, kBmpBytesPerPixel * 8, 2);
  put_le(&bmp, 34, static_cast<uint32_t>(image_size), 4);

  for (int y = 0; y < p.height; ++y) {
    char* row = bmp.data() + kBmpHeaderSize + static_cast<size_t>(p.height - 1 - y) * row_size;
    for (int x = 0; x < p.width; ++x) {
      uint16_t c;
      std::memcpy(&c, frame.data() + (y * p.width + x) * kBytesPerPixel, sizeof c);
      const int r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
      // Repeating the top bits makes full scale 255, not 248.
      row[x * 3 + 0] = static_cast<char>((b << 3) | (b >> 2));
      row[x * 3 + 1] = static_cast<char>((g << 2) | (g >> 4));
      row[x * 3 + 2] = static_cast<char>((r << 3) | (r >> 2));
    }
  }

  std::ofstream out(path, std::ios::binary);
  out.write(bmp.data(), static_cast<std::streamsize>(bmp.size()));
  out.close();
  if (!out) throw std::runtime_error(std::string("cannot write ") + path);
}

}  // namespace preflash::panel

namespace preflash::touch {
namespace {

std::mutex g_mutex;
std::deque<Event> g_events;

}  // namespace

void push(Event e) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_events.push_back(e);
}

std::optional<Event> pop() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_events.empty()) return std::nullopt;
  const Event e = g_events.front();
  g_events.pop_front();
  return e;
}

}  // namespace preflash::touch
