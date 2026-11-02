#include <algorithm>
#include <array>
#include <cstring>

#include "preflash/chips.h"
#include "preflash/fail.h"
#include "preflash/touch.h"

namespace preflash {
namespace {

constexpr std::array<uint8_t, 11> kReadCommand = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00,
                                                  0x00, 0x0e, 0x00, 0x00, 0x00};
constexpr size_t kPointsByte = 1;
constexpr size_t kLongSideByte = 2;
constexpr size_t kShortSideByte = 4;

}  // namespace

Axs15231bTouch::Axs15231bTouch(int width, int height) : width_(width), height_(height) {}

bool Axs15231bTouch::write(std::span<const uint8_t> data) {
  if (data.size() != kReadCommand.size() ||
      !std::equal(data.begin(), data.end(), kReadCommand.begin())) {
    preflash_unsupported("axs15231b touch", "the read command is the only one modelled");
  }
  asked_ = true;
  return true;
}

bool Axs15231bTouch::read(std::span<uint8_t> out) {
  std::fill(out.begin(), out.end(), 0);
  if (!asked_) return true;
  asked_ = false;
  if (const auto event = touch::pop()) {
    down_ = event->down;
    x_ = std::clamp(event->x, 0, width_ - 1);
    y_ = std::clamp(event->y, 0, height_ - 1);
  }
  if (!down_ || out.size() <= kShortSideByte + 1) return true;
  const int long_side = height_ - 1 - y_;
  out[kPointsByte] = 1;
  out[kLongSideByte] = static_cast<uint8_t>(long_side >> 8);
  out[kLongSideByte + 1] = static_cast<uint8_t>(long_side);
  out[kShortSideByte] = static_cast<uint8_t>(x_ >> 8);
  out[kShortSideByte + 1] = static_cast<uint8_t>(x_);
  return true;
}

}  // namespace preflash
