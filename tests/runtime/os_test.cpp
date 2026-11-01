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
  std::promise<void> started;
  auto received = std::async(std::launch::async, [queue, &started] {
    int value = 0;
    started.set_value();
    // Bounded, so a lost wakeup fails the test instead of hanging the future's destructor.
    const BaseType_t got = xQueueReceive(queue, &value, pdMS_TO_TICKS(5000));
    return got == pdTRUE ? value : -1;
  });
  started.get_future().wait();
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
  std::promise<void> started;
  auto sent = std::async(std::launch::async, [queue, &started] {
    const int second = 2;
    started.set_value();
    return xQueueSend(queue, &second, pdMS_TO_TICKS(5000));
  });
  started.get_future().wait();
  EXPECT_EQ(sent.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  int value = 0;
  ASSERT_EQ(xQueueReceive(queue, &value, 0), pdTRUE);
  EXPECT_EQ(value, 1);
  ASSERT_EQ(sent.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_EQ(sent.get(), pdTRUE);
  ASSERT_EQ(xQueueReceive(queue, &value, 0), pdTRUE);
  EXPECT_EQ(value, 2);
}

TEST(Tasks, DeletingItselfEndsTheTaskThere) {
  static std::atomic<int> reached{0};
  static std::atomic<bool> ran_on{false};
  reached = 0;
  ran_on = false;
  auto body = [](void*) {
    reached = 1;
    vTaskDelete(nullptr);
    ran_on = true;
  };
  ASSERT_EQ(xTaskCreate(body, "gone", 4096, nullptr, 1, nullptr), pdPASS);
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (reached == 0 && std::chrono::steady_clock::now() < give_up) vTaskDelay(1);
  ASSERT_EQ(reached, 1);
  vTaskDelay(pdMS_TO_TICKS(20));
  EXPECT_FALSE(ran_on);
}

TEST(Tasks, DeletingItselfByHandleWorksToo) {
  static std::atomic<int> reached{0};
  reached = 0;
  auto body = [](void*) {
    reached = 1;
    vTaskDelete(xTaskGetCurrentTaskHandle());
    reached = 2;
  };
  ASSERT_EQ(xTaskCreate(body, "gone", 4096, nullptr, 1, nullptr), pdPASS);
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (reached == 0 && std::chrono::steady_clock::now() < give_up) vTaskDelay(1);
  vTaskDelay(pdMS_TO_TICKS(20));
  EXPECT_EQ(reached, 1);
}

TEST(TasksDeathTest, DeletingAnotherTaskAbortsNamingTheCall) {
  auto idle = [](void*) {
    for (;;) vTaskDelay(pdMS_TO_TICKS(1000));
  };
  TaskHandle_t other = nullptr;
  ASSERT_EQ(xTaskCreate(idle, "other", 4096, nullptr, 1, &other), pdPASS);
  EXPECT_DEATH(vTaskDelete(other), "vTaskDelete");
}

TEST(Tasks, KnowsItsOwnHandleAndName) {
  static std::atomic<TaskHandle_t> seen{nullptr};
  static char name[16];
  seen = nullptr;
  auto body = [](void*) {
    std::snprintf(name, sizeof name, "%s", pcTaskGetName(nullptr));
    seen = xTaskGetCurrentTaskHandle();
    vTaskDelete(nullptr);
  };
  TaskHandle_t made = nullptr;
  ASSERT_EQ(xTaskCreate(body, "named", 4096, nullptr, 1, &made), pdPASS);
  const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!seen && std::chrono::steady_clock::now() < give_up) vTaskDelay(1);
  EXPECT_EQ(seen.load(), made);
  EXPECT_STREQ(name, "named");
  EXPECT_STREQ(pcTaskGetName(made), "named");
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

TEST(Restart, WorksWithAStandardDescriptorClosed) {
  TempState state("restart_closed");
  int status = -1;
  const std::string output = run_restart_helper("PREFLASH_HELPER_CLOSE_STDIN=1", state, &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("second run:"), std::string::npos) << output;
}

TEST(Restart, StartsCleanWithoutInheritedDescriptorsOrBlockedSignals) {
  TempState state("restart_clean");
  int status = -1;
  const std::string output = run_restart_helper("PREFLASH_HELPER_LEAK=1", state, &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("descriptor=closed"), std::string::npos) << output;
  EXPECT_NE(output.find("signals=unblocked"), std::string::npos) << output;
  EXPECT_NE(output.find("ignored=no"), std::string::npos) << output;
}
