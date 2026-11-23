#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "preflash/bus.h"

namespace {

using namespace std::chrono_literals;

// What a handler saw. Each test has its own pin, because pin state lives for the process.
struct Seen {
  std::atomic<int> calls{0};
  std::atomic<int> level{-1};
  std::atomic<int> arg_ok{0};
};

void on_edge(void* arg) {
  auto* seen = static_cast<Seen*>(arg);
  seen->level = gpio_get_level(GPIO_NUM_0);
  seen->arg_ok = 1;
  seen->calls++;
}

template <typename Done>
bool wait_for(Done done) {
  for (int i = 0; i < 400 && !done(); ++i) std::this_thread::sleep_for(5ms);
  return done();
}

void settle_quiet() { std::this_thread::sleep_for(60ms); }

void install_service() {
  const esp_err_t err = gpio_install_isr_service(0);
  ASSERT_TRUE(err == ESP_OK || err == ESP_ERR_INVALID_STATE);
}

// A button on `pin`: input, pulled up, released and quiet.
void button_pin(gpio_num_t pin, gpio_int_type_t type) {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << pin;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.intr_type = type;
  ASSERT_EQ(gpio_config(&config), ESP_OK);
  preflash::gpio_write(pin, true);
}

}  // namespace

TEST(GpioIsr, AFallingEdgeCallsItsHandler) {
  install_service();
  button_pin(GPIO_NUM_0, GPIO_INTR_NEGEDGE);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_0, on_edge, &seen), ESP_OK);
  preflash::gpio_write(0, false);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  EXPECT_EQ(seen.arg_ok, 1);
  settle_quiet();
  EXPECT_EQ(seen.calls, 1);
  gpio_isr_handler_remove(GPIO_NUM_0);
}

TEST(GpioIsr, ARisingEdgeDoesntCallANegedgeHandler) {
  install_service();
  button_pin(GPIO_NUM_1, GPIO_INTR_NEGEDGE);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_1, on_edge, &seen), ESP_OK);
  preflash::gpio_write(1, false);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  preflash::gpio_write(1, true);
  settle_quiet();
  EXPECT_EQ(seen.calls, 1);
  gpio_isr_handler_remove(GPIO_NUM_1);
}

TEST(GpioIsr, ADisabledInterruptStaysQuiet) {
  install_service();
  button_pin(GPIO_NUM_2, GPIO_INTR_ANYEDGE);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_2, on_edge, &seen), ESP_OK);
  ASSERT_EQ(gpio_intr_disable(GPIO_NUM_2), ESP_OK);
  preflash::gpio_write(2, false);
  settle_quiet();
  EXPECT_EQ(seen.calls, 0);
  ASSERT_EQ(gpio_intr_enable(GPIO_NUM_2), ESP_OK);
  preflash::gpio_write(2, true);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  gpio_isr_handler_remove(GPIO_NUM_2);
}

namespace {
void read_own_level(void* arg) {
  auto* seen = static_cast<Seen*>(arg);
  seen->level = gpio_get_level(GPIO_NUM_3);
  seen->calls++;
}
}  // namespace

TEST(GpioIsr, AHandlerCanReadTheNewLevel) {
  install_service();
  button_pin(GPIO_NUM_3, GPIO_INTR_ANYEDGE);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_3, read_own_level, &seen), ESP_OK);
  preflash::gpio_write(3, false);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  EXPECT_EQ(seen.level, 0);
  preflash::gpio_write(3, true);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 2; }));
  EXPECT_EQ(seen.level, 1);
  gpio_isr_handler_remove(GPIO_NUM_3);
}

TEST(GpioIsr, ConfigWithAnInterruptTypeNoLongerStops) {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << GPIO_NUM_4;
  config.mode = GPIO_MODE_INPUT;
  config.intr_type = GPIO_INTR_ANYEDGE;
  EXPECT_EQ(gpio_config(&config), ESP_OK);
}

namespace {
void count_only(void* arg) { static_cast<Seen*>(arg)->calls++; }
}  // namespace

TEST(GpioIsr, ALevelTypeFiresOnceOnEnteringTheLevel) {
  install_service();
  button_pin(GPIO_NUM_5, GPIO_INTR_LOW_LEVEL);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_5, count_only, &seen), ESP_OK);
  preflash::gpio_write(5, false);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  preflash::gpio_write(5, false);  // still held
  settle_quiet();
  EXPECT_EQ(seen.calls, 1);
  preflash::gpio_write(5, true);  // leaving it isn't the level
  settle_quiet();
  EXPECT_EQ(seen.calls, 1);
  preflash::gpio_write(5, false);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 2; }));
  gpio_isr_handler_remove(GPIO_NUM_5);
}

TEST(GpioIsr, AHighLevelTypeFiresOnEnteringHigh) {
  install_service();
  button_pin(GPIO_NUM_6, GPIO_INTR_HIGH_LEVEL);
  preflash::gpio_write(6, false);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_6, count_only, &seen), ESP_OK);
  preflash::gpio_write(6, true);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  settle_quiet();
  EXPECT_EQ(seen.calls, 1);
  gpio_isr_handler_remove(GPIO_NUM_6);
}

TEST(GpioIsr, ARemovedHandlerStaysQuiet) {
  install_service();
  button_pin(GPIO_NUM_7, GPIO_INTR_ANYEDGE);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_7, count_only, &seen), ESP_OK);
  ASSERT_EQ(gpio_isr_handler_remove(GPIO_NUM_7), ESP_OK);
  preflash::gpio_write(7, false);
  settle_quiet();
  EXPECT_EQ(seen.calls, 0);
}

TEST(GpioIsr, SetIntrTypeChangesWhatFires) {
  install_service();
  button_pin(GPIO_NUM_8, GPIO_INTR_NEGEDGE);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_8, count_only, &seen), ESP_OK);
  ASSERT_EQ(gpio_set_intr_type(GPIO_NUM_8, GPIO_INTR_POSEDGE), ESP_OK);
  preflash::gpio_write(8, false);
  settle_quiet();
  EXPECT_EQ(seen.calls, 0);
  preflash::gpio_write(8, true);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  gpio_isr_handler_remove(GPIO_NUM_8);
}

namespace {
std::atomic<bool> g_release_slow{false};
void slow_handler(void* arg) {
  static_cast<Seen*>(arg)->calls++;
  while (!g_release_slow) std::this_thread::sleep_for(1ms);
}
}  // namespace

TEST(GpioIsr, ASlowHandlerDoesntBlockGpioWriteOrOtherCalls) {
  install_service();
  button_pin(GPIO_NUM_9, GPIO_INTR_ANYEDGE);
  Seen seen;
  g_release_slow = false;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_9, slow_handler, &seen), ESP_OK);
  preflash::gpio_write(9, false);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  const auto start = std::chrono::steady_clock::now();
  preflash::gpio_write(9, true);
  preflash::gpio_write(9, false);
  EXPECT_EQ(gpio_get_level(GPIO_NUM_9), 0);
  EXPECT_LT(std::chrono::steady_clock::now() - start, 500ms);
  g_release_slow = true;
  ASSERT_TRUE(wait_for([&] { return seen.calls == 3; }));  // the queued edges follow, in order
  gpio_isr_handler_remove(GPIO_NUM_9);
}

TEST(GpioIsr, ErrorsFollowEspIdf) {
  const auto kNoSuchPin = static_cast<gpio_num_t>(22);
  install_service();
  EXPECT_EQ(gpio_install_isr_service(0), ESP_ERR_INVALID_STATE);
  Seen seen;
  EXPECT_EQ(gpio_isr_handler_add(kNoSuchPin, count_only, &seen), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(gpio_isr_handler_remove(kNoSuchPin), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(gpio_intr_enable(kNoSuchPin), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(gpio_intr_disable(kNoSuchPin), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(gpio_set_intr_type(kNoSuchPin, GPIO_INTR_ANYEDGE), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(gpio_set_intr_type(GPIO_NUM_10, GPIO_INTR_MAX), ESP_ERR_INVALID_ARG);
}

TEST(GpioIsr, GpioIsrRegisterIsUnsupportedNamingTheCall) {
  EXPECT_DEATH(gpio_isr_register(nullptr, nullptr, 0, nullptr), "gpio_isr_register");
}

TEST(GpioIsr, FromIsrCallsWorkInAHandler) {
  install_service();
  button_pin(GPIO_NUM_11, GPIO_INTR_NEGEDGE);
  static QueueHandle_t queue = xQueueCreate(4, sizeof(int));
  static TaskHandle_t waiter = nullptr;
  static std::atomic<int> notified{0};
  waiter = nullptr;
  std::thread([] {
    waiter = xTaskGetCurrentTaskHandle();
    notified = static_cast<int>(ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(2000)));
  }).detach();
  ASSERT_TRUE(wait_for([] { return waiter != nullptr; }));
  ASSERT_EQ(gpio_isr_handler_add(
                GPIO_NUM_11,
                [](void*) {
                  BaseType_t woken = pdFALSE;
                  int pin = 11;
                  xQueueSendFromISR(queue, &pin, &woken);
                  vTaskNotifyGiveFromISR(waiter, &woken);
                  portYIELD_FROM_ISR(woken);
                },
                nullptr),
            ESP_OK);
  preflash::gpio_write(11, false);
  int got = 0;
  ASSERT_EQ(xQueueReceive(queue, &got, pdMS_TO_TICKS(2000)), pdTRUE);
  EXPECT_EQ(got, 11);
  ASSERT_TRUE(wait_for([] { return notified == 1; }));
  gpio_isr_handler_remove(GPIO_NUM_11);
}

TEST(GpioIsr, ALowLevelArmedWhileLowFiresOnce) {
  install_service();
  button_pin(GPIO_NUM_12, GPIO_INTR_DISABLE);
  preflash::gpio_write(12, false);  // held before firmware configures it
  Seen seen;
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << GPIO_NUM_12;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  config.intr_type = GPIO_INTR_LOW_LEVEL;
  ASSERT_EQ(gpio_config(&config), ESP_OK);
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_12, count_only, &seen), ESP_OK);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  settle_quiet();
  EXPECT_EQ(seen.calls, 1);
  gpio_isr_handler_remove(GPIO_NUM_12);
}

TEST(GpioIsr, AnEdgeArmedAtItsLevelStaysQuiet) {
  install_service();
  button_pin(GPIO_NUM_13, GPIO_INTR_NEGEDGE);
  preflash::gpio_write(13, false);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_13, count_only, &seen), ESP_OK);
  settle_quiet();
  EXPECT_EQ(seen.calls, 0);
  gpio_isr_handler_remove(GPIO_NUM_13);
}

TEST(GpioIsr, ReArmingWhileHeldFiresAgain) {
  install_service();
  button_pin(GPIO_NUM_14, GPIO_INTR_LOW_LEVEL);
  preflash::gpio_write(14, false);
  Seen seen;
  ASSERT_EQ(gpio_isr_handler_add(GPIO_NUM_14, count_only, &seen), ESP_OK);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 1; }));
  ASSERT_EQ(gpio_intr_disable(GPIO_NUM_14), ESP_OK);
  ASSERT_EQ(gpio_intr_enable(GPIO_NUM_14), ESP_OK);
  ASSERT_TRUE(wait_for([&] { return seen.calls == 2; }));
  settle_quiet();
  EXPECT_EQ(seen.calls, 2);
  gpio_isr_handler_remove(GPIO_NUM_14);
}
