#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "esp_flash.h"
#include "esp_partition.h"

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kLogAddress = 0xFE0000;
constexpr uint32_t kLogSize = 0x20000;

class Flash : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("preflash_flash_" + std::to_string(::getpid()) + "_" +
                                        ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir_);
    fs::create_directories(dir_);
    setenv("PREFLASH_STATE", dir_.c_str(), 1);
  }
  void TearDown() override {
    if (part_) esp_partition_deregister_external(part_);
    fs::remove_all(dir_);
  }
  const esp_partition_t* add(uint32_t address = kLogAddress, uint32_t size = kLogSize) {
    EXPECT_EQ(esp_partition_register_external(esp_flash_default_chip, address, size, "evlog",
                                              ESP_PARTITION_TYPE_DATA,
                                              ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, &part_),
              ESP_OK);
    return part_;
  }
  fs::path dir_;
  const esp_partition_t* part_ = nullptr;
};

}  // namespace

TEST_F(Flash, ReportsSixteenMegabytes) {
  uint32_t n = 0;
  ASSERT_EQ(esp_flash_get_size(nullptr, &n), ESP_OK);
  EXPECT_EQ(n, 16777216u);
  EXPECT_EQ(esp_flash_default_chip->size, 16777216u);
}

TEST_F(Flash, RegistersAtTheEnd) {
  const esp_partition_t* p = add();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->address, kLogAddress);
  EXPECT_EQ(p->size, kLogSize);
  EXPECT_EQ(p->erase_size, 4096u);
  EXPECT_EQ(p->flash_chip, esp_flash_default_chip);
  EXPECT_STREQ(p->label, "evlog");
  const esp_partition_t* other = nullptr;
  EXPECT_EQ(esp_partition_register_external(esp_flash_default_chip, 0xFFF000, 0x2000, "bad",
                                            ESP_PARTITION_TYPE_DATA,
                                            ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, &other),
            ESP_ERR_INVALID_SIZE);
}

TEST_F(Flash, OverlapIsRejected) {
  add();
  const esp_partition_t* other = nullptr;
  EXPECT_EQ(esp_partition_register_external(esp_flash_default_chip, kLogAddress + 0x1000, 0x1000,
                                            "dup", ESP_PARTITION_TYPE_DATA,
                                            ESP_PARTITION_SUBTYPE_DATA_UNDEFINED, &other),
            ESP_ERR_INVALID_ARG);
}

TEST_F(Flash, ErasedReadsFF) {
  const esp_partition_t* p = add();
  std::vector<uint8_t> buf(8192, 0);
  ASSERT_EQ(esp_partition_read(p, 4096, buf.data(), buf.size()), ESP_OK);
  for (uint8_t b : buf) ASSERT_EQ(b, 0xFF);
}

TEST_F(Flash, WriteOnlyClearsBits) {
  const esp_partition_t* p = add();
  uint8_t a = 0x0F, b = 0xF0, out = 0x55;
  ASSERT_EQ(esp_partition_write(p, 10, &a, 1), ESP_OK);
  ASSERT_EQ(esp_partition_write(p, 10, &b, 1), ESP_OK);
  ASSERT_EQ(esp_partition_read(p, 10, &out, 1), ESP_OK);
  EXPECT_EQ(out, 0x00);
}

TEST_F(Flash, EraseSetsFF) {
  const esp_partition_t* p = add();
  uint8_t zero = 0, out = 0;
  ASSERT_EQ(esp_partition_write(p, 4100, &zero, 1), ESP_OK);
  ASSERT_EQ(esp_partition_erase_range(p, 4096, 4096), ESP_OK);
  ASSERT_EQ(esp_partition_read(p, 4100, &out, 1), ESP_OK);
  EXPECT_EQ(out, 0xFF);
}

TEST_F(Flash, EraseNeedsSectorAlignment) {
  const esp_partition_t* p = add();
  EXPECT_EQ(esp_partition_erase_range(p, 100, 4096), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_partition_erase_range(p, 4096, 100), ESP_ERR_INVALID_ARG);
}

TEST_F(Flash, BeyondThePartitionIsInvalidSize) {
  const esp_partition_t* p = add();
  uint8_t b = 0;
  EXPECT_EQ(esp_partition_read(p, kLogSize, &b, 1), ESP_ERR_INVALID_SIZE);
  EXPECT_EQ(esp_partition_write(p, kLogSize - 1, "ab", 2), ESP_ERR_INVALID_SIZE);
  EXPECT_EQ(esp_partition_erase_range(p, kLogSize, 4096), ESP_ERR_INVALID_SIZE);
}

TEST_F(Flash, SurvivesRestart) {
  const esp_partition_t* p = add();
  const uint8_t in[] = {1, 2, 3, 0x7E};
  ASSERT_EQ(esp_partition_write(p, 5000, in, sizeof in), ESP_OK);
  ASSERT_EQ(esp_partition_deregister_external(p), ESP_OK);
  part_ = nullptr;
  p = add();
  uint8_t out[4] = {};
  ASSERT_EQ(esp_partition_read(p, 5000, out, sizeof out), ESP_OK);
  EXPECT_EQ(std::vector<uint8_t>(out, out + 4), std::vector<uint8_t>(in, in + 4));
  EXPECT_TRUE(fs::exists(dir_ / "flash.bin"));
}
