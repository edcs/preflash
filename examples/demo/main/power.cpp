#include "power.h"

#include <cstdint>

#include "esp_log.h"
#include "i2c_chip.h"

namespace power {
namespace {

constexpr const char* kTag = "power";

constexpr uint8_t kAddress = 0x34;
constexpr uint8_t kStatus1 = 0x00;
constexpr uint8_t kStatus2 = 0x01;
constexpr uint8_t kCommon = 0x10;
constexpr uint8_t kGauge = 0xa4;

constexpr uint8_t kVbusGood = 0x20;
constexpr uint8_t kBatteryPresent = 0x08;
constexpr int kCurrentShift = 5;
constexpr uint8_t kCurrentMask = 0x03;
constexpr uint8_t kCurrentCharging = 0x01;
constexpr uint8_t kCurrentDischarging = 0x02;
constexpr uint8_t kChargerStateMask = 0x07;
constexpr uint8_t kChargerDone = 0x04;
constexpr uint8_t kSoftOff = 0x01;
// The power key's interrupts, the same bits in the enable and status registers.
constexpr uint8_t kIrqEnable1 = 0x41;
constexpr uint8_t kIrqStatus1 = 0x49;
constexpr uint8_t kKeyShort = 0x08;
constexpr uint8_t kKeyLong = 0x04;

I2cChip g_chip;

Charge charge_from(uint8_t status) {
  const uint8_t current = (status >> kCurrentShift) & kCurrentMask;
  if (current == kCurrentCharging) return Charge::charging;
  if (current == kCurrentDischarging) return Charge::discharging;
  return (status & kChargerStateMask) == kChargerDone ? Charge::done : Charge::none;
}

}  // namespace

void start(i2c_master_bus_handle_t bus) { g_chip.attach(bus, kAddress); }

Reading read() {
  Reading reading;
  uint8_t status[2];
  uint8_t gauge = 0;
  if (g_chip.read(kStatus1, &status[0], 1) != ESP_OK || g_chip.read(kStatus2, &status[1], 1) != ESP_OK ||
      g_chip.read(kGauge, &gauge, 1) != ESP_OK) {
    return reading;
  }
  reading.answered = true;
  reading.usb = status[0] & kVbusGood;
  reading.battery = status[0] & kBatteryPresent;
  reading.percent = gauge;
  reading.charge = reading.battery ? charge_from(status[1]) : Charge::none;
  return reading;
}

void turn_off() {
  uint8_t common = 0;
  esp_err_t err = g_chip.read(kCommon, &common, 1);
  if (err == ESP_OK) err = g_chip.write(kCommon, common | kSoftOff);
  if (err != ESP_OK) ESP_LOGW(kTag, "couldn't turn off: %s", esp_err_to_name(err));
}

void watch_key() {
  uint8_t enabled = 0;
  esp_err_t err = g_chip.read(kIrqEnable1, &enabled, 1);
  if (err == ESP_OK) err = g_chip.write(kIrqEnable1, enabled | kKeyShort | kKeyLong);
  if (err != ESP_OK) ESP_LOGW(kTag, "couldn't watch the key: %s", esp_err_to_name(err));
}

Key take_key() {
  uint8_t status = 0;
  if (g_chip.read(kIrqStatus1, &status, 1) != ESP_OK) return Key::none;
  const uint8_t seen = status & (kKeyShort | kKeyLong);
  if (!seen) return Key::none;
  // A bit clears when 1 is written to it. Until the clear lands the bit stays up, so the key
  // is reported once that write succeeds, and the next poll tries again if it doesn't.
  if (g_chip.write(kIrqStatus1, seen) != ESP_OK) return Key::none;
  return (seen & kKeyLong) ? Key::hold : Key::tap;
}

}  // namespace power
