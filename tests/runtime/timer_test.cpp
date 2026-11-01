#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

#include "esp_timer.h"

namespace {

using namespace std::chrono_literals;

std::atomic<int> g_fired{0};
void count(void* arg) { static_cast<std::atomic<int>*>(arg)->fetch_add(1); }

esp_timer_handle_t make(esp_timer_cb_t callback, void* arg, bool skip = false) {
  esp_timer_create_args_t args = {};
  args.callback = callback;
  args.arg = arg;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "test";
  args.skip_unhandled_events = skip;
  esp_timer_handle_t timer = nullptr;
  EXPECT_EQ(esp_timer_create(&args, &timer), ESP_OK);
  return timer;
}

bool wait_for(const std::atomic<int>& value, int at_least) {
  const auto give_up = std::chrono::steady_clock::now() + 5s;
  while (value < at_least) {
    if (std::chrono::steady_clock::now() > give_up) return false;
    std::this_thread::sleep_for(1ms);
  }
  return true;
}

TEST(Timer, CreateRefusesBadArguments) {
  esp_timer_handle_t timer = nullptr;
  esp_timer_create_args_t args = {};
  EXPECT_EQ(esp_timer_create(nullptr, &timer), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_timer_create(&args, &timer), ESP_ERR_INVALID_ARG);  // no callback
  args.callback = count;
  EXPECT_EQ(esp_timer_create(&args, nullptr), ESP_ERR_INVALID_ARG);
  args.dispatch_method = ESP_TIMER_MAX;
  EXPECT_EQ(esp_timer_create(&args, &timer), ESP_ERR_INVALID_ARG);
}

TEST(Timer, NullHandlesAreRefused) {
  EXPECT_EQ(esp_timer_start_once(nullptr, 1), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_timer_start_periodic(nullptr, 1), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_timer_stop(nullptr), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_timer_delete(nullptr), ESP_ERR_INVALID_ARG);
}

TEST(Timer, OnceFiresOnceWithItsArgumentAndDisarms) {
  std::atomic<int> fired{0};
  esp_timer_handle_t timer = make(count, &fired);
  ASSERT_EQ(esp_timer_start_once(timer, 1000), ESP_OK);
  ASSERT_TRUE(wait_for(fired, 1));
  std::this_thread::sleep_for(20ms);
  EXPECT_EQ(fired, 1);
  // Disarmed by firing: stopping has nothing to stop, and it can start again.
  EXPECT_EQ(esp_timer_stop(timer), ESP_ERR_INVALID_STATE);
  ASSERT_EQ(esp_timer_start_once(timer, 1000), ESP_OK);
  EXPECT_TRUE(wait_for(fired, 2));
  EXPECT_EQ(esp_timer_delete(timer), ESP_OK);
}

TEST(Timer, DoesNotFireEarly) {
  std::atomic<int> fired{0};
  esp_timer_handle_t timer = make(count, &fired);
  ASSERT_EQ(esp_timer_start_once(timer, 60 * 1000 * 1000), ESP_OK);
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(fired, 0);
  EXPECT_EQ(esp_timer_stop(timer), ESP_OK);
  EXPECT_EQ(esp_timer_delete(timer), ESP_OK);
}

TEST(Timer, PeriodicRepeatsUntilStopped) {
  std::atomic<int> fired{0};
  esp_timer_handle_t timer = make(count, &fired);
  ASSERT_EQ(esp_timer_start_periodic(timer, 2000), ESP_OK);
  ASSERT_TRUE(wait_for(fired, 3));
  ASSERT_EQ(esp_timer_stop(timer), ESP_OK);
  std::this_thread::sleep_for(10ms);
  const int settled = fired;
  std::this_thread::sleep_for(30ms);
  EXPECT_EQ(fired, settled);
  EXPECT_EQ(esp_timer_delete(timer), ESP_OK);
}

TEST(Timer, ArmedTimerRefusesToStartAgainOrBeDeleted) {
  std::atomic<int> fired{0};
  esp_timer_handle_t timer = make(count, &fired);
  ASSERT_EQ(esp_timer_start_once(timer, 60 * 1000 * 1000), ESP_OK);
  EXPECT_EQ(esp_timer_start_once(timer, 1), ESP_ERR_INVALID_STATE);
  EXPECT_EQ(esp_timer_start_periodic(timer, 1000), ESP_ERR_INVALID_STATE);
  EXPECT_EQ(esp_timer_delete(timer), ESP_ERR_INVALID_STATE);
  EXPECT_EQ(esp_timer_stop(timer), ESP_OK);
  EXPECT_EQ(esp_timer_stop(timer), ESP_ERR_INVALID_STATE);
  EXPECT_EQ(esp_timer_delete(timer), ESP_OK);
}

TEST(Timer, EarliestFiresFirstWhicheverStartedFirst) {
  static std::atomic<int> order{0};
  static std::atomic<int> late_at{0};
  static std::atomic<int> early_at{0};
  order = 0;
  esp_timer_handle_t late = make([](void*) { late_at = ++order; }, nullptr);
  esp_timer_handle_t early = make([](void*) { early_at = ++order; }, nullptr);
  ASSERT_EQ(esp_timer_start_once(late, 40 * 1000), ESP_OK);
  ASSERT_EQ(esp_timer_start_once(early, 2 * 1000), ESP_OK);
  ASSERT_TRUE(wait_for(order, 2));
  EXPECT_LT(early_at, late_at);
}

TEST(Timer, CallbackMayStopAndDeleteItsOwnTimer) {
  static esp_timer_handle_t timer;
  static std::atomic<int> done{0};
  done = 0;
  timer = make(
      [](void*) {
        EXPECT_EQ(esp_timer_stop(timer), ESP_OK);
        EXPECT_EQ(esp_timer_delete(timer), ESP_OK);
        done = 1;
      },
      nullptr);
  ASSERT_EQ(esp_timer_start_periodic(timer, 1000), ESP_OK);
  EXPECT_TRUE(wait_for(done, 1));
}

// A callback that outruns its period: the catch-up runs straight after it, as the
// device's timer task does, unless the timer was asked to skip them.
TEST(Timer, MissedPeriodsCatchUpUnlessSkipped) {
  static std::atomic<int> catching{0};
  static std::atomic<int> skipping{0};
  catching = 0;
  skipping = 0;
  esp_timer_handle_t caught = make(
      [](void*) {
        if (catching++ == 0) std::this_thread::sleep_for(50ms);
      },
      nullptr);
  ASSERT_EQ(esp_timer_start_periodic(caught, 5000), ESP_OK);
  std::this_thread::sleep_for(75ms);
  ASSERT_EQ(esp_timer_stop(caught), ESP_OK);
  EXPECT_GE(catching, 10);

  esp_timer_handle_t skipped = make(
      [](void*) {
        if (skipping++ == 0) std::this_thread::sleep_for(50ms);
      },
      nullptr, true);
  ASSERT_EQ(esp_timer_start_periodic(skipped, 5000), ESP_OK);
  std::this_thread::sleep_for(75ms);
  ASSERT_EQ(esp_timer_stop(skipped), ESP_OK);
  EXPECT_LE(skipping, 8);
}

}  // namespace
