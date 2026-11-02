#include "preflash/panel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

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
  uint64_t frames = 0;
  std::deque<std::chrono::steady_clock::time_point> recent;  // frames in the last second
};

Pixels& pixels() {
  static Pixels p;
  return p;
}

// With the mutex held.
void forget_old_frames(Pixels& p, std::chrono::steady_clock::time_point now) {
  while (!p.recent.empty() && now - p.recent.front() >= std::chrono::seconds(1)) {
    p.recent.pop_front();
  }
}

void put_le(std::vector<char>* out, size_t at, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) (*out)[at + i] = static_cast<char>(value >> (8 * i));
}

}  // namespace

void draw(int x1, int y1, int x2, int y2, const void* rgb565, bool last_of_frame) {
  Pixels& p = pixels();
  if (last_of_frame) {
    // Read under the lock, so that the times in the list stay in order.
    std::lock_guard<std::mutex> lock(p.mutex);
    const auto now = std::chrono::steady_clock::now();
    ++p.frames;
    forget_old_frames(p, now);
    p.recent.push_back(now);
  }
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

uint64_t frames_drawn() {
  Pixels& p = pixels();
  std::lock_guard<std::mutex> lock(p.mutex);
  return p.frames;
}

int fps() {
  Pixels& p = pixels();
  std::lock_guard<std::mutex> lock(p.mutex);
  forget_old_frames(p, std::chrono::steady_clock::now());
  return static_cast<int>(p.recent.size());
}

// Once firmware sets a level, the PWM peripheral owns the backlight pin, so a plain
// output level no longer matters. Setting the pin up as a GPIO again takes it back, which
// is what the direction epoch tells. A held pad ignores the peripheral until it is released.
std::atomic<int> g_pwm_epoch{-1};

void set_brightness(int pct) {
  g_brightness = std::clamp(pct, 0, 100);
  g_pwm_epoch = gpio_direction_epoch(board().backlight.pin);
}

namespace {

// What the screen shows, given the enable pin's level if the board has one. The level
// is passed in so a status line reads the pin once.
int shown(std::optional<bool> enable_high) {
  if (enable_high && !*enable_high) return 0;
  const auto& bl = board().backlight;
  const bool pwm_owns = g_pwm_epoch == gpio_direction_epoch(bl.pin);
  if (gpio_is_held(bl.pin) || (gpio_is_output(bl.pin) && !pwm_owns)) {
    const bool on = gpio_read(bl.pin) != bl.active_low;
    return on ? g_brightness.load() : 0;
  }
  return g_brightness;
}

std::optional<bool> enable_level() {
  const std::optional<int> enable = board().backlight.enable_pin;
  if (!enable) return std::nullopt;
  return gpio_read(*enable);
}

}  // namespace

// A separate enable pin cuts the driver's power, so it wins over any PWM level. The
// backlight pin itself, when firmware holds or drives it directly, is plain on or off.
int brightness() { return shown(enable_level()); }

int brightness_level() { return g_brightness; }

std::string backlight_status() {
  const std::optional<bool> level = enable_level();
  std::string status =
      "brightness: " + std::to_string(shown(level)) + " (pwm " + std::to_string(g_brightness);
  if (level) {
    status += ", enable gpio" + std::to_string(*board().backlight.enable_pin) +
              (*level ? " high" : " low");
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

std::vector<uint8_t> copy_frame() {
  Pixels& p = pixels();
  std::lock_guard<std::mutex> lock(p.mutex);
  return p.data;
}

void save_bmp(const char* path) {
  Pixels& p = pixels();
  const std::vector<uint8_t> frame = copy_frame();

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

bool press(int x, int y, int ms) {
  if (ms < 0 || x < 0 || y < 0 || x >= board().width || y >= board().height) return false;
  static std::mutex gesture;
  std::lock_guard<std::mutex> one_at_a_time(gesture);
  push({x, y, true});
  std::this_thread::sleep_for(std::chrono::milliseconds(ms));
  push({x, y, false});
  return true;
}

}  // namespace preflash::touch
