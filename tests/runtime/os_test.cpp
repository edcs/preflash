#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <future>
#include <string>

#include "esp_err.h"
#include "esp_system.h"
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
  for (int i = 0; i < 1000 && !wake.started; ++i) vTaskDelay(1);
  ASSERT_TRUE(wake.started);
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

TEST(Queues, ReceiveBlocksUntilAnotherThreadSends) {
  QueueHandle_t queue = xQueueCreate(1, sizeof(int));
  auto received = std::async(std::launch::async, [queue] {
    int value = 0;
    const BaseType_t got = xQueueReceive(queue, &value, portMAX_DELAY);
    return got == pdTRUE ? value : -1;
  });
  // Still waiting: nothing has been sent.
  EXPECT_EQ(received.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  const int sent = 42;
  ASSERT_EQ(xQueueSend(queue, &sent, 0), pdTRUE);
  ASSERT_EQ(received.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(received.get(), 42);
}

TEST(Queues, SendBlocksUntilAnotherThreadReceives) {
  QueueHandle_t queue = xQueueCreate(1, sizeof(int));
  const int first = 1;
  ASSERT_EQ(xQueueSend(queue, &first, 0), pdTRUE);
  auto sent = std::async(std::launch::async, [queue] {
    const int second = 2;
    return xQueueSend(queue, &second, portMAX_DELAY);
  });
  EXPECT_EQ(sent.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  int value = 0;
  ASSERT_EQ(xQueueReceive(queue, &value, 0), pdTRUE);
  EXPECT_EQ(value, 1);
  ASSERT_EQ(sent.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(sent.get(), pdTRUE);
  ASSERT_EQ(xQueueReceive(queue, &value, 0), pdTRUE);
  EXPECT_EQ(value, 2);
}

TEST(Queues, NullHandleAbortsNamingTheCall) {
  int value = 0;
  EXPECT_DEATH(xQueueSend(nullptr, &value, 0), "xQueueSend");
  EXPECT_DEATH(xQueueReceive(nullptr, &value, 0), "xQueueReceive");
}

TEST(Tasks, NullHandleAbortsNamingTheCall) {
  EXPECT_DEATH(xTaskNotifyGive(nullptr), "xTaskNotifyGive");
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

TEST(ErrorCheck, KeepsCallerVariableNamedLikeOldTemporary) {
  esp_err_t err_rc_ = ESP_OK;
  ESP_ERROR_CHECK(err_rc_);
  err_rc_ = ESP_FAIL;
  EXPECT_DEATH(ESP_ERROR_CHECK(err_rc_), "ESP_FAIL");
}

TEST(Unsupported, AbortsNamingTheCall) {
  EXPECT_DEATH(preflash_unsupported("x", "y"), "x");
}

TEST(Reset, ReasonIsPowerOnUntilARestart) {
  const char* before = std::getenv("PREFLASH_RESTARTED");
  const std::string saved = before ? before : "";
  unsetenv("PREFLASH_RESTARTED");
  EXPECT_EQ(esp_reset_reason(), ESP_RST_POWERON);
  setenv("PREFLASH_RESTARTED", "1", 1);
  EXPECT_EQ(esp_reset_reason(), ESP_RST_SW);
  if (before) {
    setenv("PREFLASH_RESTARTED", saved.c_str(), 1);
  } else {
    unsetenv("PREFLASH_RESTARTED");
  }
}

namespace {

// A folder that goes away however the test ends.
struct TempState {
  explicit TempState(const std::string& name)
      : path(std::filesystem::temp_directory_path() /
             ("preflash_" + name + "_" + std::to_string(getpid()))) {
    std::filesystem::create_directories(path);
  }
  ~TempState() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
  std::filesystem::path path;
};

// Runs the restart helper to the end and returns what it printed.
std::string run_restart_helper(const std::string& environment, const TempState& state,
                               int* status) {
  const std::string command = "PREFLASH_STATE='" + state.path.string() + "' " + environment +
                              " '" + RESTART_HELPER + "' 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    *status = -1;
    return "";
  }
  std::string output;
  char buffer[256];
  while (std::fgets(buffer, sizeof buffer, pipe)) output += buffer;
  *status = pclose(pipe);
  return output;
}

}  // namespace

TEST(Restart, ReExecsTheBinary) {
  TempState state("restart");
  int status = -1;
  const std::string output = run_restart_helper("", state, &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("second run: state=" + state.path.string()), std::string::npos) << output;
  // 3 is ESP_RST_SW: the second run knows it was restarted.
  EXPECT_NE(output.find("reason=3"), std::string::npos) << output;
}

TEST(Restart, StartsCleanWithoutInheritedDescriptorsOrBlockedSignals) {
  TempState state("restart_clean");
  int status = -1;
  const std::string output = run_restart_helper("PREFLASH_HELPER_LEAK=1", state, &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("descriptor=closed"), std::string::npos) << output;
  EXPECT_NE(output.find("signals=unblocked"), std::string::npos) << output;
}
