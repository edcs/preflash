#include "preflash/panel.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "preflash/board.h"
#include "preflash/bus.h"
#include "preflash/crash.h"
#include "preflash/touch.h"
#include "../winlink/frame_buffer.h"

namespace preflash::panel {
namespace {

constexpr int kBytesPerPixel = 2;
constexpr int kBmpHeaderSize = 54;
constexpr int kBmpBytesPerPixel = 3;
// Firmware drawing through esp_lcd finishes a frame with every bitmap, which can be a strip of
// a few rows, so the window's copy of the whole panel is made at most this often on firmware's
// thread. LVGL at the panel's rate comes in under it, and the main thread catches up the rest.
constexpr auto kShareEvery = std::chrono::milliseconds(8);

std::atomic<int> g_brightness{100};
std::atomic<int> g_rotation{0};

struct Pixels {
  std::mutex mutex;
  int width = board().width;
  int height = board().height;
  std::vector<uint8_t> data = std::vector<uint8_t>(width * height * kBytesPerPixel);
  bool changed = true;
  uint64_t frames = 0;
  std::deque<std::chrono::steady_clock::time_point> recent;  // frames in the last second
  std::optional<winlink::FrameBuffer> shared;                // the window's copy, if it has one
  std::chrono::steady_clock::time_point last_shared;         // when that copy was last written
  bool held_back = false;  // the panel holds a finished frame the window's copy lacks
};

Pixels& pixels() {
  static Pixels p;
  return p;
}

// Who's told of each finished frame. The list's lock is held for as long as a subscriber takes
// to note a number, and for no longer by anyone else, and with nobody listening it isn't taken.
struct FrameListeners {
  std::mutex mutex;
  std::map<int, FrameSubscriber> by_number;
  int next = 0;
  std::atomic<int> count{0};
};

FrameListeners& listeners() {
  static FrameListeners* l = new FrameListeners;  // threads outlive static destructors
  return *l;
}

// With the mutex held.
void forget_old_frames(Pixels& p, std::chrono::steady_clock::time_point now) {
  while (!p.recent.empty() && now - p.recent.front() >= std::chrono::seconds(1)) {
    p.recent.pop_front();
  }
}

// Firmware can fault inside draw, on a bad bitmap, with the lock held for good. Everything
// that reads the pixels for the window or a client goes on after that, and the frame it reads
// is as good as it will get.
std::unique_lock<std::mutex> read_lock(Pixels& p) {
  std::unique_lock<std::mutex> lock(p.mutex, std::defer_lock);
  while (!lock.try_lock() && !crash::happened()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return lock;
}

void put_le(std::vector<char>* out, size_t at, uint32_t value, int bytes) {
  for (int i = 0; i < bytes; ++i) (*out)[at + i] = static_cast<char>(value >> (8 * i));
}

}  // namespace

namespace {

// Puts the bitmap in the panel and returns the number of the frame it finished, or 0.
uint64_t draw_locked(int x1, int y1, int x2, int y2, const void* rgb565, bool last_of_frame) {
  Pixels& p = pixels();
  const int source_stride = (x2 - x1) * kBytesPerPixel;
  const int left = std::max(x1, 0);
  const int right = std::min(x2, p.width);
  const int top = std::max(y1, 0);
  const int bottom = std::min(y2, p.height);

  const auto* source = static_cast<const uint8_t*>(rgb565);
  // One lock for the pixels and the count, so that a frame counted can also be read.
  std::lock_guard<std::mutex> lock(p.mutex);
  uint64_t finished = 0;
  if (last_of_frame) {
    const auto now = std::chrono::steady_clock::now();
    finished = ++p.frames;
    forget_old_frames(p, now);
    p.recent.push_back(now);
  }
  if (left < right && top < bottom) {
    for (int y = top; y < bottom; ++y) {
      const uint8_t* from = source + (y - y1) * source_stride + (left - x1) * kBytesPerPixel;
      uint8_t* to = p.data.data() + (y * p.width + left) * kBytesPerPixel;
      std::memcpy(to, from, (right - left) * kBytesPerPixel);
    }
    p.changed = true;
  }
  if (!p.shared) return finished;
  // Whole frames only, so the window never shows one half drawn: a frame held back is let go
  // once firmware starts on the next one, which will finish in its turn.
  if (!last_of_frame) {
    p.held_back = false;
    return finished;
  }
  const auto now = std::chrono::steady_clock::now();
  if (now - p.last_shared < kShareEvery) {
    p.held_back = true;
    return finished;
  }
  p.shared->write(p.data.data());
  p.last_shared = now;
  p.held_back = false;
  return finished;
}

}  // namespace

void draw(int x1, int y1, int x2, int y2, const void* rgb565, bool last_of_frame) {
  const uint64_t finished = draw_locked(x1, y1, x2, y2, rgb565, last_of_frame);
  if (finished == 0) return;
  // After the pixels' lock is let go, so a subscriber never holds up a reader of the panel.
  FrameListeners& l = listeners();
  if (l.count.load(std::memory_order_acquire) == 0) return;
  std::lock_guard<std::mutex> lock(l.mutex);
  for (const auto& entry : l.by_number) entry.second(finished);
}

int subscribe_frames(FrameSubscriber subscriber) {
  FrameListeners& l = listeners();
  std::lock_guard<std::mutex> lock(l.mutex);
  const int number = l.next++;
  l.by_number.emplace(number, std::move(subscriber));
  l.count.store(static_cast<int>(l.by_number.size()), std::memory_order_release);
  return number;
}

void unsubscribe_frames(int subscription) {
  FrameListeners& l = listeners();
  std::lock_guard<std::mutex> lock(l.mutex);
  l.by_number.erase(subscription);
  l.count.store(static_cast<int>(l.by_number.size()), std::memory_order_release);
}

void share_frames(const std::string& path) {
  Pixels& p = pixels();
  auto shared = winlink::FrameBuffer::create(path, p.width, p.height);
  std::lock_guard<std::mutex> lock(p.mutex);
  p.shared.emplace(std::move(shared));
}

void stop_sharing() {
  Pixels& p = pixels();
  std::lock_guard<std::mutex> lock(p.mutex);
  p.shared.reset();
}

void share_pending() {
  Pixels& p = pixels();
  // Never waits: firmware that crashed inside draw holds the lock for good.
  std::unique_lock<std::mutex> lock(p.mutex, std::try_to_lock);
  if (!lock.owns_lock() || !p.shared || !p.held_back || crash::happened()) return;
  p.shared->write(p.data.data());
  p.last_shared = std::chrono::steady_clock::now();
  p.held_back = false;
}

uint64_t frames_drawn() {
  Pixels& p = pixels();
  const auto lock = read_lock(p);
  return p.frames;
}

int fps() {
  Pixels& p = pixels();
  const auto lock = read_lock(p);
  forget_old_frames(p, std::chrono::steady_clock::now());
  return static_cast<int>(p.recent.size());
}

void set_rotation(int degrees) { g_rotation = degrees; }

int rotation() { return g_rotation; }

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
  const Backlight& bl = board().backlight;
  if (bl.enable_expander_bit) return expander_level(*bl.enable_expander_bit);
  if (bl.enable_pin) return gpio_read(*bl.enable_pin);
  return std::nullopt;
}

std::string enable_name() {
  const Backlight& bl = board().backlight;
  return bl.enable_expander_bit ? "exio" + std::to_string(*bl.enable_expander_bit)
                                : "gpio" + std::to_string(*bl.enable_pin);
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
  if (level) status += ", enable " + enable_name() + (*level ? " high" : " low");
  return status + ")";
}

std::string window_title() {
  const char* project = std::getenv("PREFLASH_PROJECT");
  return (project ? std::string(project) : board().name) + " - backlight " +
         std::to_string(brightness()) + "%";
}

bool take_frame(std::vector<uint8_t>* out) {
  Pixels& p = pixels();
  const auto lock = read_lock(p);
  if (!p.changed) return false;
  *out = p.data;
  p.changed = false;
  return true;
}

std::vector<uint8_t> copy_frame(uint64_t* number) {
  Pixels& p = pixels();
  const auto lock = read_lock(p);
  if (number) *number = p.frames;
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

void pointer(int x, int y, bool down) {
  if (crash::happened()) return;
  push({std::clamp(x, 0, board().width - 1), std::clamp(y, 0, board().height - 1), down});
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
