#include <map>
#include <mutex>

#include "driver/gpio.h"
#include "preflash/bus.h"

namespace {

constexpr int kOutputBit = 2;

struct Pin {
  bool output = false;   // the output driver is on
  bool latch = false;    // what gpio_set_level last asked for
  bool outside = false;  // what something off the chip drives, seen when output is off
  bool held = false;     // the pad keeps its level whatever the rest of this says
  bool level = false;    // what's on the pad
};

std::mutex g_mutex;
std::map<int, Pin> g_pins;

bool exists(gpio_num_t pin) {
  return pin >= GPIO_NUM_0 && pin < GPIO_NUM_MAX && (pin <= GPIO_NUM_21 || pin >= GPIO_NUM_26);
}

void settle(Pin* pin) {
  if (!pin->held) pin->level = pin->output ? pin->latch : pin->outside;
}

}  // namespace

namespace preflash {

void gpio_write(int pin, bool level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Pin& driven = g_pins[pin];
  driven.outside = level;
  driven.level = level;
}

bool gpio_read(int pin) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_pins[pin].level;
}

}  // namespace preflash

extern "C" {

esp_err_t gpio_reset_pin(gpio_num_t gpio_num) {
  return gpio_set_direction(gpio_num, GPIO_MODE_DISABLE);
}

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  Pin& pin = g_pins[gpio_num];
  pin.output = (mode & kOutputBit) != 0;
  settle(&pin);
  return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  Pin& pin = g_pins[gpio_num];
  pin.latch = level != 0;
  settle(&pin);
  return ESP_OK;
}

esp_err_t gpio_hold_en(gpio_num_t gpio_num) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pins[gpio_num].held = true;
  return ESP_OK;
}

esp_err_t gpio_hold_dis(gpio_num_t gpio_num) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  Pin& pin = g_pins[gpio_num];
  pin.held = false;
  settle(&pin);
  return ESP_OK;
}

}  // extern "C"
