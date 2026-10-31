#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <span>
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

i2c_master_bus_handle_t new_bus() {
  const i2c_master_bus_config_t config = {};
  i2c_master_bus_handle_t bus = nullptr;
  EXPECT_EQ(i2c_new_master_bus(&config, &bus), ESP_OK);
  return bus;
}

i2c_master_dev_handle_t new_device(uint16_t address) {
  i2c_device_config_t config = {};
  config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
  config.device_address = address;
  config.scl_speed_hz = 100000;
  i2c_master_dev_handle_t device = nullptr;
  EXPECT_EQ(i2c_master_bus_add_device(new_bus(), &config, &device), ESP_OK);
  return device;
}

}  // namespace

TEST(I2c, TransmitReceiveReachesTheDevice) {
  std::vector<uint8_t> log;
  i2c_bus().add(0x11, std::make_unique<FakeDevice>(&log));
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
  i2c_bus().add(0x12, std::make_unique<FakeDevice>(&log));

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
  EXPECT_EQ(i2c_master_transmit(device, command, sizeof command, kTimeoutMs), ESP_ERR_NOT_FOUND);
  EXPECT_EQ(i2c_master_transmit_receive(device, command, sizeof command, &answer, 1, kTimeoutMs),
            ESP_ERR_NOT_FOUND);
}

TEST(I2c, RefusedWriteIsAnError) {
  std::vector<uint8_t> log;
  i2c_bus().add(0x13, std::make_unique<FakeDevice>(&log, false));
  const uint8_t command[] = {0x00};
  EXPECT_EQ(i2c_master_transmit(new_device(0x13), command, sizeof command, kTimeoutMs),
            ESP_ERR_NOT_FOUND);
}

TEST(I2c, DevicesAddedLaterAreSeenByExistingHandles) {
  // A wrapper that replaces a chip must see traffic from handles already open.
  std::vector<uint8_t> log;
  i2c_master_dev_handle_t device = new_device(0x14);
  i2c_bus().add(0x14, std::make_unique<FakeDevice>(&log));
  const uint8_t command[] = {0x01};
  EXPECT_EQ(i2c_master_transmit(device, command, sizeof command, kTimeoutMs), ESP_OK);
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

TEST(Gpio, RejectsPinsTheChipDoesntHave) {
  EXPECT_EQ(gpio_set_level(static_cast<gpio_num_t>(22), 1), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(gpio_set_direction(static_cast<gpio_num_t>(49), GPIO_MODE_OUTPUT), ESP_ERR_INVALID_ARG);
}
