#include "monitor.h"

#include <atomic>
#include <cstdio>
#include <mutex>

#include "board.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace monitor {
namespace {

constexpr const char* kTag = "monitor";
constexpr int64_t kPeriodUs = 500'000;

std::mutex g_mutex;
Snapshot g_latest;
TaskHandle_t g_task = nullptr;
std::atomic<bool> g_hung{false};

void follow_rotation(Snapshot& snapshot, int was_showing) {
  snapshot.shown_degrees = was_showing;
  const int degrees = snapshot.motion.degrees;
  if (!snapshot.motion.answered || degrees == was_showing || !board::can_turn_to(degrees)) return;
  {
    board::Lock lock;
    board::turn_to(degrees);
  }
  snapshot.shown_degrees = degrees;
  std::printf("rotation=%d\n", degrees);
}

void sample() {
  Snapshot snapshot;
  time_t rtc_time;
  snapshot.rtc = rtc::read(&rtc_time);
  snapshot.motion = motion::read();
  if (board::has_power_chip()) snapshot.power = power::read();
  follow_rotation(snapshot, latest().shown_degrees);

  std::lock_guard lock(g_mutex);
  g_latest = snapshot;
}

void wake_task(void*) { xTaskNotifyGive(g_task); }

void monitor_task(void*) {
  ESP_ERROR_CHECK(esp_task_wdt_add(nullptr));
  for (;;) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    while (g_hung) vTaskDelay(pdMS_TO_TICKS(100));
    sample();
    ESP_ERROR_CHECK(esp_task_wdt_reset());
  }
}

}  // namespace

void start() {
  sample();
  ESP_ERROR_CHECK(xTaskCreate(monitor_task, "monitor", 4096, nullptr, 3, &g_task) == pdPASS ? ESP_OK : ESP_FAIL);

  esp_timer_create_args_t timer = {};
  timer.callback = wake_task;
  timer.name = "monitor";
  esp_timer_handle_t handle = nullptr;
  ESP_ERROR_CHECK(esp_timer_create(&timer, &handle));
  ESP_ERROR_CHECK(esp_timer_start_periodic(handle, kPeriodUs));
}

Snapshot latest() {
  std::lock_guard lock(g_mutex);
  return g_latest;
}

void hang() {
  ESP_LOGW(kTag, "no more watchdog feeding");
  g_hung = true;
}

}  // namespace monitor
