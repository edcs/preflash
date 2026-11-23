#include "buttons.h"

#include <atomic>
#include <cstdio>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "power.h"

namespace buttons {
namespace {

constexpr const char* kTag = "buttons";

constexpr gpio_num_t kBootPin = GPIO_NUM_0;
constexpr int kPressed = 0;  // BOOT pulls the pin low
// A real switch bounces for a few milliseconds, so edges this close are one press.
constexpr int64_t kBounceUs = 20'000;
constexpr int kPollMs = 50;

TaskHandle_t g_task = nullptr;
std::atomic<uint32_t> g_boot_presses{0};
std::atomic<int64_t> g_last_edge_us{-kBounceUs};
std::atomic<uint32_t> g_power_taps{0};
std::atomic<uint32_t> g_power_holds{0};
std::atomic<bool> g_hold_turns_off{true};
bool g_has_power_key = false;

// Runs in interrupt context: count the press and wake the task, and do nothing slow.
void IRAM_ATTR on_boot_edge(void*) {
  const int64_t now = esp_timer_get_time();
  if (now - g_last_edge_us.load() < kBounceUs) return;
  g_last_edge_us = now;
  g_boot_presses.fetch_add(1);
  BaseType_t woken = pdFALSE;
  vTaskNotifyGiveFromISR(g_task, &woken);
  portYIELD_FROM_ISR(woken);
}

void watch_boot() {
  gpio_config_t pin = {};
  pin.pin_bit_mask = 1ULL << kBootPin;
  pin.mode = GPIO_MODE_INPUT;
  pin.pull_up_en = GPIO_PULLUP_ENABLE;
  pin.intr_type = GPIO_INTR_NEGEDGE;
  ESP_ERROR_CHECK(gpio_config(&pin));
  // Another driver may have installed the service already, which is fine.
  const esp_err_t installed = gpio_install_isr_service(0);
  ESP_ERROR_CHECK(installed == ESP_ERR_INVALID_STATE ? ESP_OK : installed);
  ESP_ERROR_CHECK(gpio_isr_handler_add(kBootPin, on_boot_edge, nullptr));
}

void buttons_task(void*) {
  uint32_t reported = 0;
  for (;;) {
    // A press wakes this at once, and otherwise it looks at the power key every poll.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPollMs));

    const uint32_t presses = g_boot_presses.load();
    if (presses != reported) {
      reported = presses;
      std::printf("buttons: BOOT pressed, %lu so far\n", static_cast<unsigned long>(presses));
    }

    if (!g_has_power_key) continue;
    const power::Key key = power::take_key();
    if (key == power::Key::tap) {
      std::printf("buttons: PWR tapped, %lu so far\n", static_cast<unsigned long>(++g_power_taps));
    } else if (key == power::Key::hold) {
      std::printf("buttons: PWR held, %lu so far\n", static_cast<unsigned long>(++g_power_holds));
      // The chip only reports the hold. What it means is up to the firmware.
      if (g_hold_turns_off) {
        std::printf("buttons: turning off\n");
        power::turn_off();
      }
    }
  }
}

}  // namespace

void start(bool has_power_chip) {
  g_has_power_key = has_power_chip;
  if (has_power_chip) power::watch_key();
  ESP_ERROR_CHECK(xTaskCreate(buttons_task, "buttons", 4096, nullptr, 3, &g_task) == pdPASS ? ESP_OK : ESP_FAIL);
  watch_boot();
  ESP_LOGI(kTag, "watching BOOT%s", has_power_chip ? " and PWR" : "");
}

State latest() {
  State state;
  state.boot_down = gpio_get_level(kBootPin) == kPressed;
  state.boot_presses = g_boot_presses;
  state.has_power_key = g_has_power_key;
  state.power_taps = g_power_taps;
  state.power_holds = g_power_holds;
  state.hold_turns_off = g_hold_turns_off;
  return state;
}

void set_hold_turns_off(bool on) { g_hold_turns_off = on; }

}  // namespace buttons
