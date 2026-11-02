#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "preflash/bus.h"

using preflash::gpio_read;
using preflash::gpio_write;
using preflash::i2c_bus;
using preflash::I2cDevice;

namespace {

constexpr int kTimeoutMs = 100;

// Keeps what it was sent, in the order it arrived, and answers with a fixed byte.
class FakeDevice : public I2cDevice {
 public:
  explicit FakeDevice(std::vector<uint8_t>* log, bool acks = true) : log_(log), acks_(acks) {}
  bool write(std::span<const uint8_t> data) override {
    log_->insert(log_->end(), data.begin(), data.end());
    return acks_;
  }
  bool read(std::span<uint8_t> out) override {
    for (uint8_t& byte : out) byte = 0xa5;
    return acks_;
  }

 private:
  std::vector<uint8_t>* log_;
  bool acks_;
};

// Puts a fake on the bus and takes it off again at the end of the test, before
// the log it writes to goes out of scope.
class Attached {
 public:
  Attached(uint8_t address, std::vector<uint8_t>* log, bool acks = true) : address_(address) {
    i2c_bus().add(address, std::make_unique<FakeDevice>(log, acks));
  }
  ~Attached() { i2c_bus().take(address_); }
  Attached(const Attached&) = delete;
  Attached& operator=(const Attached&) = delete;

 private:
  uint8_t address_;
};

i2c_master_bus_handle_t new_bus() {
  const i2c_master_bus_config_t config = {};
  i2c_master_bus_handle_t bus = nullptr;
  EXPECT_EQ(i2c_new_master_bus(&config, &bus), ESP_OK);
  return bus;
}

i2c_master_dev_handle_t add_device(i2c_master_bus_handle_t bus, uint16_t address) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = 100000;
  i2c_master_dev_handle_t device = nullptr;
  EXPECT_EQ(i2c_master_bus_add_device(bus, &config, &device), ESP_OK);
  return device;
}

i2c_master_dev_handle_t new_device(uint16_t address) { return add_device(new_bus(), address); }

i2c_master_dev_handle_t device_on(int port, uint16_t address) {
  i2c_master_bus_config_t config = {};
  config.i2c_port = port;
  i2c_master_bus_handle_t bus = nullptr;
  EXPECT_EQ(i2c_new_master_bus(&config, &bus), ESP_OK);
  return add_device(bus, address);
}

}  // namespace

TEST(I2c, TransmitReceiveReachesTheDevice) {
  std::vector<uint8_t> log;
  Attached fake(0x11, &log);
  i2c_master_dev_handle_t device = new_device(0x11);

  const uint8_t command[] = {0x04, 0x05};
  uint8_t answer[2] = {};
  ASSERT_EQ(i2c_master_transmit_receive(device, command, sizeof command, answer, sizeof answer,
                                        kTimeoutMs),
            ESP_OK);
  EXPECT_EQ(log, (std::vector<uint8_t>{0x04, 0x05}));
  EXPECT_EQ(answer[0], 0xa5);
  EXPECT_EQ(answer[1], 0xa5);
}

TEST(I2c, TransmitReachesTheDevice) {
  std::vector<uint8_t> log;
  Attached fake(0x12, &log);

  const uint8_t command[] = {0x09};
  ASSERT_EQ(i2c_master_transmit(new_device(0x12), command, sizeof command, kTimeoutMs), ESP_OK);
  EXPECT_EQ(log, (std::vector<uint8_t>{0x09}));
}

TEST(I2c, NoDeviceAtAddressIsAnError) {
  // Adding succeeds with nothing there, as it does on hardware: the first
  // transfer is what finds out.
  i2c_master_dev_handle_t device = new_device(0x77);
  const uint8_t command[] = {0x00};
  uint8_t answer = 0;
  EXPECT_EQ(i2c_master_transmit(device, command, sizeof command, kTimeoutMs), ESP_ERR_INVALID_STATE);
  EXPECT_EQ(i2c_master_transmit_receive(device, command, sizeof command, &answer, 1, kTimeoutMs),
            ESP_ERR_INVALID_STATE);
}

TEST(I2c, RefusedWriteIsAnError) {
  std::vector<uint8_t> log;
  Attached fake(0x13, &log, false);
  const uint8_t command[] = {0x00};
  EXPECT_EQ(i2c_master_transmit(new_device(0x13), command, sizeof command, kTimeoutMs),
            ESP_ERR_INVALID_STATE);
}

TEST(I2c, DevicesAddedLaterAreSeenByExistingHandles) {
  // A wrapper that replaces a chip must see traffic from handles already open.
  std::vector<uint8_t> log;
  i2c_master_dev_handle_t device = new_device(0x14);
  Attached fake(0x14, &log);
  const uint8_t command[] = {0x01};
  EXPECT_EQ(i2c_master_transmit(device, command, sizeof command, kTimeoutMs), ESP_OK);
}

TEST(I2c, TakeHandsTheDeviceBackForAWrapper) {
  // Forwards to the device it was built around, as a wrapper would.
  class Wrapper : public I2cDevice {
   public:
    explicit Wrapper(std::unique_ptr<I2cDevice> inner) : inner_(std::move(inner)) {}
    bool write(std::span<const uint8_t> data) override { return inner_->write(data); }
    bool read(std::span<uint8_t> out) override { return inner_->read(out); }

   private:
    std::unique_ptr<I2cDevice> inner_;
  };

  std::vector<uint8_t> log;
  Attached fake(0x15, &log);
  i2c_master_dev_handle_t device = new_device(0x15);

  std::unique_ptr<I2cDevice> taken = i2c_bus().take(0x15);
  ASSERT_NE(taken, nullptr);
  EXPECT_EQ(i2c_bus().device(0x15), nullptr);
  const uint8_t command[] = {0x02};
  EXPECT_EQ(i2c_master_transmit(device, command, sizeof command, kTimeoutMs),
            ESP_ERR_INVALID_STATE);
  EXPECT_TRUE(log.empty());

  i2c_bus().add(0x15, std::make_unique<Wrapper>(std::move(taken)));
  EXPECT_EQ(i2c_master_transmit(device, command, sizeof command, kTimeoutMs), ESP_OK);
  EXPECT_EQ(log, (std::vector<uint8_t>{0x02}));
}

TEST(I2c, TakeFromAnEmptyAddressGivesNothing) {
  EXPECT_EQ(i2c_bus().take(0x16), nullptr);
}

TEST(I2c, RejectsBadArguments) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = 0x80;  // doesn't fit in seven bits
  i2c_master_dev_handle_t device = nullptr;
  EXPECT_EQ(i2c_master_bus_add_device(new_bus(), &config, &device), ESP_ERR_INVALID_ARG);
  config.device_address = 0x10;
  EXPECT_EQ(i2c_master_bus_add_device(nullptr, &config, &device), ESP_ERR_INVALID_ARG);
  const uint8_t command[] = {0x00};
  EXPECT_EQ(i2c_master_transmit(nullptr, command, sizeof command, kTimeoutMs),
            ESP_ERR_INVALID_ARG);
}

TEST(I2c, ADeviceOnlyAnswersOnItsOwnBus) {
  std::vector<uint8_t> log;
  i2c_bus(1).add(0x22, std::make_unique<FakeDevice>(&log));
  i2c_master_dev_handle_t right = device_on(1, 0x22);
  i2c_master_dev_handle_t wrong = device_on(0, 0x22);
  const uint8_t byte = 0x01;
  EXPECT_EQ(i2c_master_transmit(right, &byte, 1, kTimeoutMs), ESP_OK);
  EXPECT_EQ(i2c_master_transmit(wrong, &byte, 1, kTimeoutMs), ESP_ERR_INVALID_STATE);
  EXPECT_EQ(log.size(), 1u);
  i2c_bus(1).take(0x22);
}

TEST(I2c, TheSameAddressCanSitOnBothBuses) {
  std::vector<uint8_t> first, second;
  Attached on_zero(0x23, &first);
  i2c_bus(1).add(0x23, std::make_unique<FakeDevice>(&second));
  const uint8_t byte = 0x02;
  EXPECT_EQ(i2c_master_transmit(device_on(1, 0x23), &byte, 1, kTimeoutMs), ESP_OK);
  EXPECT_TRUE(first.empty());
  EXPECT_EQ(second.size(), 1u);
  i2c_bus(1).take(0x23);
}

TEST(I2c, APortTheChipLacksIsAnError) {
  i2c_master_bus_config_t config = {};
  config.i2c_port = 2;
  i2c_master_bus_handle_t bus = nullptr;
  EXPECT_EQ(i2c_new_master_bus(&config, &bus), ESP_ERR_INVALID_ARG);
}

TEST(I2c, AnAutomaticPortRunsOutOfBuses) {
  i2c_master_bus_config_t config = {};
  config.i2c_port = -1;
  i2c_master_bus_handle_t bus = nullptr;
  EXPECT_EQ(i2c_new_master_bus(&config, &bus), ESP_OK);
  EXPECT_EQ(i2c_new_master_bus(&config, &bus), ESP_OK);
  EXPECT_EQ(i2c_new_master_bus(&config, &bus), ESP_ERR_NOT_FOUND);
}

TEST(Gpio, OutputPinFollowsSetLevel) {
  EXPECT_EQ(gpio_reset_pin(GPIO_NUM_10), ESP_OK);
  EXPECT_EQ(gpio_set_direction(GPIO_NUM_10, GPIO_MODE_OUTPUT), ESP_OK);
  EXPECT_EQ(gpio_set_level(GPIO_NUM_10, 1), ESP_OK);
  EXPECT_TRUE(gpio_read(10));
  EXPECT_EQ(gpio_set_level(GPIO_NUM_10, 0), ESP_OK);
  EXPECT_FALSE(gpio_read(10));
}

TEST(Gpio, InputPinIgnoresSetLevelUntilItBecomesAnOutput) {
  // Firmware that forgets gpio_set_direction leaves a real pin undriven.
  gpio_reset_pin(GPIO_NUM_11);
  gpio_set_level(GPIO_NUM_11, 1);
  EXPECT_FALSE(gpio_read(11));
  gpio_set_direction(GPIO_NUM_11, GPIO_MODE_OUTPUT);
  EXPECT_TRUE(gpio_read(11));
}

TEST(Gpio, HeldPinKeepsItsLevel) {
  gpio_reset_pin(GPIO_NUM_12);
  gpio_set_direction(GPIO_NUM_12, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_12, 1);
  EXPECT_EQ(gpio_hold_en(GPIO_NUM_12), ESP_OK);
  gpio_set_level(GPIO_NUM_12, 0);
  EXPECT_TRUE(gpio_read(12));
  EXPECT_EQ(gpio_hold_dis(GPIO_NUM_12), ESP_OK);
  EXPECT_FALSE(gpio_read(12));
}

TEST(Gpio, WriteAndReadWorkWithoutFirmware) {
  gpio_write(13, true);
  EXPECT_TRUE(gpio_read(13));
  gpio_write(13, false);
  EXPECT_FALSE(gpio_read(13));
}

TEST(Gpio, ResettingAPinLeavesWhatDrivesItFromOutside) {
  gpio_write(14, true);
  gpio_reset_pin(GPIO_NUM_14);
  EXPECT_TRUE(gpio_read(14));
}

TEST(Gpio, FirmwareOutputBeatsAnOutsideLevel) {
  gpio_reset_pin(GPIO_NUM_15);
  gpio_set_direction(GPIO_NUM_15, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_15, 1);
  gpio_write(15, false);
  EXPECT_TRUE(gpio_read(15));
}

TEST(Gpio, OpenDrainHighLetsGoOfTheLine) {
  gpio_reset_pin(GPIO_NUM_16);
  gpio_set_direction(GPIO_NUM_16, GPIO_MODE_OUTPUT_OD);
  gpio_set_level(GPIO_NUM_16, 1);
  gpio_write(16, false);
  EXPECT_FALSE(gpio_read(16));
  gpio_write(16, true);
  EXPECT_TRUE(gpio_read(16));
  gpio_set_level(GPIO_NUM_16, 0);
  EXPECT_FALSE(gpio_read(16));
}

TEST(Gpio, AnUndrivenPinReadsLowWithoutAPull) {
  ASSERT_EQ(gpio_set_direction(GPIO_NUM_5, GPIO_MODE_INPUT), ESP_OK);
  EXPECT_EQ(gpio_get_level(GPIO_NUM_5), 0);
}

TEST(Gpio, APullUpHoldsAnIdleInputHigh) {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << GPIO_NUM_6;
  config.mode = GPIO_MODE_INPUT;
  config.pull_up_en = GPIO_PULLUP_ENABLE;
  ASSERT_EQ(gpio_config(&config), ESP_OK);
  EXPECT_EQ(gpio_get_level(GPIO_NUM_6), 1);
}

TEST(Gpio, SomethingOutsideBeatsAPullUp) {
  ASSERT_EQ(gpio_set_direction(GPIO_NUM_7, GPIO_MODE_INPUT), ESP_OK);
  ASSERT_EQ(gpio_set_pull_mode(GPIO_NUM_7, GPIO_PULLUP_ONLY), ESP_OK);
  EXPECT_EQ(gpio_get_level(GPIO_NUM_7), 1);
  gpio_write(GPIO_NUM_7, false);
  EXPECT_EQ(gpio_get_level(GPIO_NUM_7), 0);
}

TEST(Gpio, APullDownBeatsNothingButReadsLow) {
  ASSERT_EQ(gpio_set_direction(GPIO_NUM_8, GPIO_MODE_INPUT), ESP_OK);
  ASSERT_EQ(gpio_pullup_en(GPIO_NUM_8), ESP_OK);
  ASSERT_EQ(gpio_pullup_dis(GPIO_NUM_8), ESP_OK);
  ASSERT_EQ(gpio_pulldown_en(GPIO_NUM_8), ESP_OK);
  EXPECT_EQ(gpio_get_level(GPIO_NUM_8), 0);
  ASSERT_EQ(gpio_pulldown_dis(GPIO_NUM_8), ESP_OK);
  ASSERT_EQ(gpio_pullup_en(GPIO_NUM_8), ESP_OK);
  EXPECT_EQ(gpio_get_level(GPIO_NUM_8), 1);
}

TEST(Gpio, RejectsAPullModeThatDoesntExist) {
  EXPECT_EQ(gpio_set_pull_mode(GPIO_NUM_8, static_cast<gpio_pull_mode_t>(9)), ESP_ERR_INVALID_ARG);
}

TEST(Gpio, AnOutputOnlyPinReadsZero) {
  ASSERT_EQ(gpio_set_direction(GPIO_NUM_9, GPIO_MODE_OUTPUT), ESP_OK);
  ASSERT_EQ(gpio_set_level(GPIO_NUM_9, 1), ESP_OK);
  EXPECT_EQ(gpio_get_level(GPIO_NUM_9), 0);
  ASSERT_EQ(gpio_set_direction(GPIO_NUM_9, GPIO_MODE_INPUT_OUTPUT), ESP_OK);
  EXPECT_EQ(gpio_get_level(GPIO_NUM_9), 1);
}

TEST(Gpio, ConfigSetsEveryPinInTheMask) {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << GPIO_NUM_10 | 1ULL << GPIO_NUM_11;
  config.mode = GPIO_MODE_OUTPUT;
  ASSERT_EQ(gpio_config(&config), ESP_OK);
  EXPECT_TRUE(preflash::gpio_is_output(GPIO_NUM_10));
  EXPECT_TRUE(preflash::gpio_is_output(GPIO_NUM_11));
}

TEST(Gpio, ConfigRejectsAnEmptyMaskAndMissingPins) {
  gpio_config_t config = {};
  config.mode = GPIO_MODE_INPUT;
  EXPECT_EQ(gpio_config(&config), ESP_ERR_INVALID_ARG);
  config.pin_bit_mask = 1ULL << 22;  // the ESP32-S3 has no GPIO 22
  EXPECT_EQ(gpio_config(&config), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(gpio_config(nullptr), ESP_ERR_INVALID_ARG);
}

TEST(GpioDeathTest, ConfigNamesInterruptsItCantRaise) {
  gpio_config_t config = {};
  config.pin_bit_mask = 1ULL << GPIO_NUM_12;
  config.mode = GPIO_MODE_INPUT;
  config.intr_type = GPIO_INTR_NEGEDGE;
  EXPECT_DEATH(gpio_config(&config), "gpio_config");
}

TEST(Gpio, RejectsPinsTheChipDoesntHave) {
  EXPECT_EQ(gpio_set_level(static_cast<gpio_num_t>(22), 1), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(gpio_set_direction(static_cast<gpio_num_t>(49), GPIO_MODE_OUTPUT), ESP_ERR_INVALID_ARG);
}
