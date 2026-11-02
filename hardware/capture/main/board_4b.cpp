// The ESP32-S3-Touch-LCD-4B. Pins and the touch reset follow Waveshare's board package.
#include "steps.h"

namespace {

constexpr int kSda = 47, kScl = 48;
constexpr uint32_t kBusHz = 100'000;
constexpr uint8_t kExpanderOut = 0x01, kExpanderConfig = 0x03;
constexpr uint8_t kResetHigh = 1 << 5, kTouchInt = 1 << 6;

// EXIO5 resets the panel and touch, and EXIO6 is touch's interrupt line. Holding that low through
// the reset sets the GT911's address to 0x5d. Each output starts high, as the chip's power-on has it.
constexpr Step touch_reset[] = {
    set(kExpanderOut, static_cast<uint8_t>(~kTouchInt)),
    set(kExpanderConfig, static_cast<uint8_t>(~(kResetHigh | kTouchInt)), 200),
    set(kExpanderOut, static_cast<uint8_t>(~(kTouchInt | kResetHigh)), 200),
    set(kExpanderOut, static_cast<uint8_t>(~kTouchInt), 200),
    set(kExpanderConfig, static_cast<uint8_t>(~kResetHigh)),
};

constexpr Bus buses[] = {{0, kSda, kScl}};

const Chip chips[] = {
    {"tca9554", 0, 0x20, kBusHz, kTca9554},
    {"tca9554", 0, 0x20, kBusHz, touch_reset},
    {"es8311", 0, 0x18, kBusHz, kEs8311},
    {"es7210", 0, 0x40, kBusHz, kEs7210},
    {"gt911", 0, 0x5d, kBusHz, kGt911},
    {"axp2101", 0, 0x34, kBusHz, kAxp2101},
    {"pcf85063", 0, 0x51, kBusHz, kPcf85063},
    {"qmi8658", 0, 0x6b, kBusHz, kQmi8658},
};

}  // namespace

const Board kBoard = {"ESP32-S3-Touch-LCD-4B", buses, chips};
