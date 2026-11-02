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

}  // namespace power
