#include "motion.h"

#include <cstdint>
#include <cstdlib>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_chip.h"

namespace motion {
namespace {

constexpr uint8_t kAddress = 0x6b;
constexpr uint8_t kWhoAmI = 0x00;
constexpr uint8_t kCtrl1 = 0x02;
constexpr uint8_t kCtrl2 = 0x03;
constexpr uint8_t kCtrl7 = 0x08;
constexpr uint8_t kAccelX = 0x35;
constexpr uint8_t kReset = 0x60;

constexpr uint8_t kExpectedId = 0x05;
constexpr uint8_t kResetCommand = 0xb0;
constexpr uint8_t kAutoIncrement = 0x40;
constexpr uint8_t kPlusMinus2gAt125Hz = 0x06;
constexpr uint8_t kAccelEnable = 0x01;
constexpr int kLsbPerG = 16384;
constexpr int kResetSettleMs = 15;

I2cChip g_chip;
bool g_configured = false;

// Can be asked again and again, so a sensor that comes back is picked up.
bool configure() {
  uint8_t id = 0;
  if (g_chip.read(kWhoAmI, &id, 1) != ESP_OK || id != kExpectedId) return false;
  if (g_chip.write(kReset, kResetCommand) != ESP_OK) return false;
  vTaskDelay(pdMS_TO_TICKS(kResetSettleMs));
  return g_chip.write(kCtrl1, kAutoIncrement) == ESP_OK && g_chip.write(kCtrl2, kPlusMinus2gAt125Hz) == ESP_OK &&
         g_chip.write(kCtrl7, kAccelEnable) == ESP_OK;
}

int degrees_from(int x, int y) {
  if (std::abs(x) > std::abs(y)) return x > 0 ? 90 : 270;
  return y >= 0 ? 0 : 180;
}

}  // namespace

void start(i2c_master_bus_handle_t bus) {
  g_chip.attach(bus, kAddress);
  g_configured = configure();
}

Reading read() {
  Reading reading;
  if (!g_configured) g_configured = configure();
  uint8_t raw[4];
  if (!g_configured || g_chip.read(kAccelX, raw, sizeof raw) != ESP_OK) {
    g_configured = false;
    return reading;
  }
  reading.answered = true;
  reading.x_mg = static_cast<int16_t>(raw[0] | raw[1] << 8) * 1000 / kLsbPerG;
  reading.y_mg = static_cast<int16_t>(raw[2] | raw[3] << 8) * 1000 / kLsbPerG;
  reading.degrees = degrees_from(reading.x_mg, reading.y_mg);
  return reading;
}

}  // namespace motion
