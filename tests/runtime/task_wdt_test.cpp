#include <gtest/gtest.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#include "esp_task_wdt.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

using namespace std::chrono_literals;

constexpr esp_task_wdt_config_t config(uint32_t timeout_ms, bool panic) {
  return {timeout_ms, 0, panic};
}

TEST(TaskWatchdog, ReconfigureRefusesBadArguments) {
  EXPECT_EQ(esp_task_wdt_reconfigure(nullptr), ESP_ERR_INVALID_ARG);
  esp_task_wdt_config_t too_many_cores = {5000, 1 << 2, false};
  EXPECT_EQ(esp_task_wdt_reconfigure(&too_many_cores), ESP_ERR_INVALID_ARG);
}

TEST(TaskWatchdogDeathTest, ZeroTimeoutAbortsNamingTheCall) {
  auto zero = config(0, false);
  EXPECT_DEATH(esp_task_wdt_reconfigure(&zero), "esp_task_wdt_reconfigure");
}

TEST(TaskWatchdog, ReconfigureAcceptsTheIdleTasksOfBothCores) {
  esp_task_wdt_config_t both = {5000, (1 << 0) | (1 << 1), false};
  EXPECT_EQ(esp_task_wdt_reconfigure(&both), ESP_OK);
}

TEST(TaskWatchdog, SubscriptionFollowsTheDevice) {
  auto quiet = config(60000, false);
  ASSERT_EQ(esp_task_wdt_reconfigure(&quiet), ESP_OK);
  EXPECT_EQ(esp_task_wdt_reset(), ESP_ERR_NOT_FOUND);
  EXPECT_EQ(esp_task_wdt_delete(nullptr), ESP_ERR_NOT_FOUND);
  ASSERT_EQ(esp_task_wdt_add(nullptr), ESP_OK);
  EXPECT_EQ(esp_task_wdt_add(nullptr), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_task_wdt_add(xTaskGetCurrentTaskHandle()), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_task_wdt_reset(), ESP_OK);
  EXPECT_EQ(esp_task_wdt_delete(nullptr), ESP_OK);
  EXPECT_EQ(esp_task_wdt_reset(), ESP_ERR_NOT_FOUND);
}

void quiet_task(void*) {
  for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
}

TEST(TaskWatchdogDeathTest, PanicNamesTheTaskThatStoppedResetting) {
  EXPECT_DEATH(
      {
        auto strict = config(30, true);
        esp_task_wdt_reconfigure(&strict);
        TaskHandle_t task = nullptr;
        xTaskCreate(quiet_task, "stuck_one", 4096, nullptr, 1, &task);
        esp_task_wdt_add(task);
        std::this_thread::sleep_for(5s);
      },
      "task watchdog.*stuck_one");
}

TEST(TaskWatchdogDeathTest, WithoutPanicItReportsAndCarriesOn) {
  EXPECT_DEATH(
      {
        auto gentle = config(20, false);
        esp_task_wdt_reconfigure(&gentle);
        TaskHandle_t task = nullptr;
        xTaskCreate(quiet_task, "gentle_one", 4096, nullptr, 1, &task);
        esp_task_wdt_add(task);
        std::this_thread::sleep_for(100ms);
        std::fputs("carried on\n", stderr);
        std::abort();
      },
      "gentle_one(.|\n)*carried on");
}

TEST(TaskWatchdog, TaskThatKeepsResettingSurvives) {
  auto strict = config(40, true);
  ASSERT_EQ(esp_task_wdt_reconfigure(&strict), ESP_OK);
  ASSERT_EQ(esp_task_wdt_add(nullptr), ESP_OK);
  const auto until = std::chrono::steady_clock::now() + 150ms;
  while (std::chrono::steady_clock::now() < until) {
    ASSERT_EQ(esp_task_wdt_reset(), ESP_OK);
    std::this_thread::sleep_for(5ms);
  }
  EXPECT_EQ(esp_task_wdt_delete(nullptr), ESP_OK);
  auto quiet = config(60000, false);
  EXPECT_EQ(esp_task_wdt_reconfigure(&quiet), ESP_OK);
}

TEST(TaskWatchdog, UnsubscribingStopsTheWatch) {
  auto strict = config(20, true);
  ASSERT_EQ(esp_task_wdt_reconfigure(&strict), ESP_OK);
  ASSERT_EQ(esp_task_wdt_add(nullptr), ESP_OK);
  ASSERT_EQ(esp_task_wdt_delete(nullptr), ESP_OK);
  std::this_thread::sleep_for(60ms);
}

}  // namespace
