// What each chip is asked. These stick to the registers firmware uses and the chip models cover,
// and they stay gentle: nothing here writes to the AXP2101, sets the RTC or leaves a chip changed
// in a way that outlives a power cut.
#include "steps.h"

namespace {

constexpr uint8_t kIn = 0x00, kOut = 0x01, kPolarity = 0x02, kConfig = 0x03;

constexpr Step tca9554[] = {
    get(kOut),
    get(kPolarity),
    get(kConfig),
    get(kIn),
    get(kOut, 2),  // no auto-increment, so the same register twice
    keep(kPolarity),
    set(kPolarity, 0xaa),
    get(kPolarity),
    get(kIn),  // inverted by the polarity above
    put_back(kPolarity),
};

constexpr uint8_t kVolume = 0x17;

constexpr Step es8311[] = {
    get(0x00),        // reset
    get(0x01),        // clock manager
    get(0xfd, 3),     // the chip's ids and version
    keep(kVolume),
    set(kVolume, 0xa5),
    get(kVolume),
    put_back(kVolume),
};

constexpr uint8_t kMic1Gain = 0x43;

constexpr Step es7210[] = {
    get(0x00),     // reset
    get(0x3d, 3),  // the chip's ids and version
    keep(kMic1Gain),
    set(kMic1Gain, 0x0a),
    get(kMic1Gain),
    put_back(kMic1Gain),
};

constexpr uint16_t kTouchStatus = 0x814e;

constexpr Step gt911[] = {
    get16(0x8140, 6),  // product id and firmware
    get16(0x8146, 4),  // resolution
    get16(0x8047),     // config version
    get16(kTouchStatus),
    set16(kTouchStatus, 0x00),
    wait(20),
    get16(kTouchStatus),
};

constexpr Step axp2101[] = {
    get(0x00),     // power status
    get(0x01),     // charger status
    get(0x03),     // chip id
    get(0x10),     // common config, whose bit 0 powers the board off
    get(0x61, 4),  // the charger's settings
    get(0xa4),     // battery percentage
};

constexpr uint8_t kRamByte = 0x03;

constexpr Step pcf85063[] = {
    get(0x00, 18),  // every register
    get(0x11, 2),   // the last one, then the wrap to 0x00
    keep(kRamByte),
    set(kRamByte, 0x5a),
    get(kRamByte),
    put_back(kRamByte),
    get(kRamByte),
};

constexpr uint8_t kCtrl1 = 0x02, kCtrl2 = 0x03, kCtrl7 = 0x08, kStatus0 = 0x2e, kAccel = 0x35, kReset = 0x60;
constexpr uint8_t kAutoIncrement = 0x40, kBigEndian = 0x20;
constexpr uint8_t kFourGAt500Hz = 0x14, kAccelOn = 0x01, kResetCommand = 0xb0;

constexpr Step qmi8658[] = {
    get(0x00),  // who am I
    get(0x01),  // revision
    get(kCtrl1),
    set(kCtrl1, 0x00),
    get(0x00, 2),  // no auto-increment, so the same register twice
    set(kCtrl1, kAutoIncrement),
    get(0x00, 2),
    set(kCtrl2, kFourGAt500Hz),
    get(kCtrl2),
    get(kStatus0),
    set(kCtrl7, kAccelOn, 100),
    get(kStatus0),
    get(kAccel, 6),
    set(kCtrl1, kAutoIncrement | kBigEndian),
    get(kCtrl1),
    get(kAccel, 6),
    set(kCtrl7, 0x00),
    set(kReset, kResetCommand, 50),
    get(0x00),
    get(kCtrl1),
    get(kCtrl2),
    get(kCtrl7),
};

// The read command from Waveshare's examples, which asks for a 14-byte reply.
constexpr Step touch_read = {
    .write = {0xb5, 0xab, 0xa5, 0x5a, 0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00}, .write_length = 11, .read_length = 14, .wait_ms = 50};

constexpr Step axs15231b_touch[] = {touch_read, touch_read, touch_read};

}  // namespace

const std::span<const Step> kTca9554 = tca9554;
const std::span<const Step> kEs8311 = es8311;
const std::span<const Step> kEs7210 = es7210;
const std::span<const Step> kGt911 = gt911;
const std::span<const Step> kAxp2101 = axp2101;
const std::span<const Step> kPcf85063 = pcf85063;
const std::span<const Step> kQmi8658 = qmi8658;
const std::span<const Step> kAxs15231bTouch = axs15231b_touch;
