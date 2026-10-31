#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "preflash/fail.h"

namespace {

struct Wake {
  std::atomic<uint32_t> taken{0};
  std::atomic<bool> started{false};
};

void waiter(void* arg) {
  auto* wake = static_cast<Wake*>(arg);
  wake->started = true;
  wake->taken = ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  vTaskDelay(portMAX_DELAY / 2);
}

}  // namespace

TEST(Tasks, CreateRunsAndNotifyWakes) {
  Wake wake;
  TaskHandle_t handle = nullptr;
  ASSERT_EQ(xTaskCreate(waiter, "waiter", 2048, &wake, 1, &handle), pdPASS);
  ASSERT_NE(handle, nullptr);
  while (!wake.started) vTaskDelay(1);
  EXPECT_EQ(wake.taken, 0u);
  xTaskNotifyGive(handle);
  for (int i = 0; i < 1000 && wake.taken == 0; ++i) vTaskDelay(1);
  EXPECT_EQ(wake.taken, 1u);
}

TEST(Tasks, NotifyTakeTimesOutWithZero) {
  EXPECT_EQ(ulTaskNotifyTake(pdTRUE, 5), 0u);
}

TEST(Tasks, DelayWaitsAtLeastTheTicks) {
  const auto start = std::chrono::steady_clock::now();
  vTaskDelay(pdMS_TO_TICKS(50));
  const auto took = std::chrono::steady_clock::now() - start;
  EXPECT_GE(took, std::chrono::milliseconds(50));
}

TEST(Queues, KeepsOrderAndTimesOut) {
  struct Item { int a; char b; };
  QueueHandle_t queue = xQueueCreate(4, sizeof(Item));
  ASSERT_NE(queue, nullptr);
  for (int i = 0; i < 3; ++i) {
    Item item{i, static_cast<char>('a' + i)};
    ASSERT_EQ(xQueueSend(queue, &item, 0), pdTRUE);
  }
  for (int i = 0; i < 3; ++i) {
    Item item{};
    ASSERT_EQ(xQueueReceive(queue, &item, 0), pdTRUE);
    EXPECT_EQ(item.a, i);
    EXPECT_EQ(item.b, 'a' + i);
  }
  Item item{};
  EXPECT_EQ(xQueueReceive(queue, &item, 10), pdFALSE);
}

TEST(Queues, FullQueueRefusesWithoutWaiting) {
  QueueHandle_t queue = xQueueCreate(1, sizeof(int));
  int value = 1;
  EXPECT_EQ(xQueueSend(queue, &value, 0), pdTRUE);
  EXPECT_EQ(xQueueSend(queue, &value, 0), pdFALSE);
}

TEST(Timer, IsMonotonicMicroseconds) {
  const int64_t first = esp_timer_get_time();
  vTaskDelay(pdMS_TO_TICKS(5));
  const int64_t second = esp_timer_get_time();
  EXPECT_GE(first, 0);
  EXPECT_GE(second - first, 5000);
}

TEST(Errors, NameKnownCodes) {
  EXPECT_STREQ(esp_err_to_name(ESP_ERR_NOT_FOUND), "ESP_ERR_NOT_FOUND");
  EXPECT_STREQ(esp_err_to_name(ESP_OK), "ESP_OK");
}

TEST(ErrorCheck, AbortsNamingTheCall) {
  EXPECT_DEATH(ESP_ERROR_CHECK(ESP_FAIL), "ESP_FAIL");
}

TEST(ErrorCheck, PassesOk) {
  ESP_ERROR_CHECK(ESP_OK);
}

TEST(Unsupported, AbortsNamingTheCall) {
  EXPECT_DEATH(preflash_unsupported("x", "y"), "x");
}

TEST(Restart, ReExecsTheBinary) {
  namespace fs = std::filesystem;
  const fs::path state = fs::temp_directory_path() / ("preflash_restart_" + std::to_string(getpid()));
  fs::create_directories(state);
  const std::string command =
      "PREFLASH_STATE=" + state.string() + " " + RESTART_HELPER + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  ASSERT_NE(pipe, nullptr);
  std::string output;
  char buffer[256];
  while (std::fgets(buffer, sizeof buffer, pipe)) output += buffer;
  EXPECT_EQ(pclose(pipe), 0);
  EXPECT_NE(output.find("second run: state=" + state.string()), std::string::npos) << output;
  // 3 is ESP_RST_SW: the second run knows it was restarted.
  EXPECT_NE(output.find("reason=3"), std::string::npos) << output;
  fs::remove_all(state);
}
