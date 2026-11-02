// The ESP32-S3-Touch-LCD-3.49 V2. Pins and the reset follow Waveshare's V2 examples.
#include "steps.h"

namespace {

constexpr int kSensorSda = 47, kSensorScl = 48;
constexpr int kTouchSda = 17, kTouchScl = 18;
constexpr uint32_t kSensorBusHz = 100'000;
constexpr uint32_t kTouchBusHz = 300'000;
constexpr uint8_t kExpanderOut = 0x01, kExpanderConfig = 0x03;
constexpr uint8_t kBacklightEnable = 1 << 1;
constexpr uint8_t kPanelReset = 1 << 5;  // active low

// Touch sits behind the panel's reset. The backlight stays off, because there's nothing to see.
constexpr Step touch_reset[] = {
    set(kExpanderOut, kPanelReset),  // levels first, so that the line never dips
    set(kExpanderConfig, static_cast<uint8_t>(~(kBacklightEnable | kPanelReset))),
    set(kExpanderOut, 0x00, 250),
    set(kExpanderOut, kPanelReset, 300),
};

constexpr Bus buses[] = {{0, kSensorSda, kSensorScl}, {1, kTouchSda, kTouchScl}};

const Chip chips[] = {
    {"tca9554", 0, 0x20, kSensorBusHz, kTca9554},
    {"tca9554", 0, 0x20, kSensorBusHz, touch_reset},
    {"qmi8658", 0, 0x6b, kSensorBusHz, kQmi8658},
    {"pcf85063", 0, 0x51, kSensorBusHz, kPcf85063},
    {"axs15231b", 1, 0x3b, kTouchBusHz, kAxs15231bTouch},
};

}  // namespace

const Board kBoard = {"ESP32-S3-Touch-LCD-3.49-V2", buses, chips};
