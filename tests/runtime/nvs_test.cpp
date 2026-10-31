#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <vector>

#include "nvs.h"
#include "nvs_flash.h"

namespace fs = std::filesystem;

namespace {

class Nvs : public ::testing::Test {
 protected:
  void SetUp() override {
    dir_ = fs::temp_directory_path() / ("preflash_nvs_" + std::to_string(::getpid()) + "_" +
                                        ::testing::UnitTest::GetInstance()->current_test_info()->name());
    fs::remove_all(dir_);
    setenv("PREFLASH_STATE", dir_.c_str(), 1);
    ASSERT_EQ(nvs_flash_init(), ESP_OK);
  }
  void TearDown() override {
    nvs_flash_deinit();
    fs::remove_all(dir_);
  }
  fs::path dir_;
};

}  // namespace

TEST_F(Nvs, BlobRoundTripsAcrossOpen) {
  const std::vector<uint8_t> in = {1, 2, 3, 0, 255};
  nvs_handle_t h;
  ASSERT_EQ(nvs_open("ns", NVS_READWRITE, &h), ESP_OK);
  ASSERT_EQ(nvs_set_blob(h, "key", in.data(), in.size()), ESP_OK);
  ASSERT_EQ(nvs_commit(h), ESP_OK);
  nvs_close(h);

  ASSERT_EQ(nvs_open("ns", NVS_READONLY, &h), ESP_OK);
  std::vector<uint8_t> out(in.size());
  size_t size = out.size();
  ASSERT_EQ(nvs_get_blob(h, "key", out.data(), &size), ESP_OK);
  EXPECT_EQ(size, in.size());
  EXPECT_EQ(out, in);
  nvs_close(h);
  EXPECT_TRUE(fs::exists(dir_ / "nvs" / "ns" / "key.bin"));
}

TEST_F(Nvs, MissingKeyIsNotFound) {
  nvs_handle_t h;
  ASSERT_EQ(nvs_open("ns", NVS_READWRITE, &h), ESP_OK);
  size_t size = 0;
  EXPECT_EQ(nvs_get_blob(h, "nope", nullptr, &size), ESP_ERR_NVS_NOT_FOUND);
  nvs_close(h);
  EXPECT_EQ(nvs_open("never_written", NVS_READONLY, &h), ESP_ERR_NVS_NOT_FOUND);
}

TEST_F(Nvs, SizeQueryWithNullOut) {
  nvs_handle_t h;
  ASSERT_EQ(nvs_open("ns", NVS_READWRITE, &h), ESP_OK);
  const char data[7] = "abcdef";
  ASSERT_EQ(nvs_set_blob(h, "k", data, 7), ESP_OK);
  ASSERT_EQ(nvs_commit(h), ESP_OK);
  size_t size = 0;
  EXPECT_EQ(nvs_get_blob(h, "k", nullptr, &size), ESP_OK);
  EXPECT_EQ(size, 7u);
  char small[2];
  size_t small_size = sizeof small;
  EXPECT_EQ(nvs_get_blob(h, "k", small, &small_size), ESP_ERR_NVS_INVALID_LENGTH);
  EXPECT_EQ(small_size, 7u);
  nvs_close(h);
}

TEST_F(Nvs, ReadOnlyWriteFails) {
  nvs_handle_t h;
  ASSERT_EQ(nvs_open("ns", NVS_READWRITE, &h), ESP_OK);
  nvs_close(h);
  ASSERT_EQ(nvs_open("ns", NVS_READONLY, &h), ESP_OK);
  EXPECT_EQ(nvs_set_blob(h, "k", "x", 1), ESP_ERR_NVS_READ_ONLY);
  EXPECT_EQ(nvs_erase_key(h, "k"), ESP_ERR_NVS_READ_ONLY);
  EXPECT_EQ(nvs_commit(h), ESP_OK);
  nvs_close(h);
}

TEST_F(Nvs, CommitIsRequired) {
  nvs_handle_t h;
  ASSERT_EQ(nvs_open("ns", NVS_READWRITE, &h), ESP_OK);
  ASSERT_EQ(nvs_set_blob(h, "k", "x", 1), ESP_OK);
  // The writer sees its own write before the commit.
  size_t size = 0;
  EXPECT_EQ(nvs_get_blob(h, "k", nullptr, &size), ESP_OK);
  nvs_close(h);

  ASSERT_EQ(nvs_open("ns", NVS_READWRITE, &h), ESP_OK);
  EXPECT_EQ(nvs_get_blob(h, "k", nullptr, &size), ESP_ERR_NVS_NOT_FOUND);
  nvs_close(h);
}

TEST_F(Nvs, EraseKeyRemovesAfterCommit) {
  nvs_handle_t h;
  ASSERT_EQ(nvs_open("ns", NVS_READWRITE, &h), ESP_OK);
  ASSERT_EQ(nvs_set_blob(h, "k", "x", 1), ESP_OK);
  ASSERT_EQ(nvs_commit(h), ESP_OK);
  ASSERT_EQ(nvs_erase_key(h, "k"), ESP_OK);
  ASSERT_EQ(nvs_commit(h), ESP_OK);
  size_t size = 0;
  EXPECT_EQ(nvs_get_blob(h, "k", nullptr, &size), ESP_ERR_NVS_NOT_FOUND);
  EXPECT_EQ(nvs_erase_key(h, "k"), ESP_ERR_NVS_NOT_FOUND);
  nvs_close(h);
}

TEST_F(Nvs, LongOrUnsafeNamesAreRefused) {
  nvs_handle_t h;
  EXPECT_EQ(nvs_open("0123456789abcdef", NVS_READWRITE, &h), ESP_ERR_NVS_KEY_TOO_LONG);
  EXPECT_EQ(nvs_open("", NVS_READWRITE, &h), ESP_ERR_NVS_INVALID_NAME);
}

TEST_F(Nvs, NamesAreCaseSensitiveAndMayHoldSlashes) {
  nvs_handle_t h;
  ASSERT_EQ(nvs_open("NS", NVS_READWRITE, &h), ESP_OK);
  ASSERT_EQ(nvs_set_blob(h, "Key", "a", 1), ESP_OK);
  ASSERT_EQ(nvs_set_blob(h, "key", "bb", 2), ESP_OK);
  ASSERT_EQ(nvs_set_blob(h, "../x", "c", 1), ESP_OK);
  ASSERT_EQ(nvs_commit(h), ESP_OK);
  nvs_close(h);

  size_t size = 0;
  ASSERT_EQ(nvs_open("NS", NVS_READONLY, &h), ESP_OK);
  EXPECT_EQ(nvs_get_blob(h, "Key", nullptr, &size), ESP_OK);
  EXPECT_EQ(size, 1u);
  EXPECT_EQ(nvs_get_blob(h, "key", nullptr, &size), ESP_OK);
  EXPECT_EQ(size, 2u);
  EXPECT_EQ(nvs_get_blob(h, "../x", nullptr, &size), ESP_OK);
  nvs_close(h);
  EXPECT_EQ(nvs_open("ns", NVS_READONLY, &h), ESP_ERR_NVS_NOT_FOUND);
  EXPECT_FALSE(fs::exists(dir_ / "x.bin"));
}

TEST_F(Nvs, EraseWipesEverything) {
  nvs_handle_t h;
  ASSERT_EQ(nvs_open("ns", NVS_READWRITE, &h), ESP_OK);
  ASSERT_EQ(nvs_set_blob(h, "k", "x", 1), ESP_OK);
  ASSERT_EQ(nvs_commit(h), ESP_OK);
  nvs_close(h);

  ASSERT_EQ(nvs_flash_deinit(), ESP_OK);
  ASSERT_EQ(nvs_flash_erase(), ESP_OK);
  EXPECT_EQ(nvs_open("ns", NVS_READONLY, &h), ESP_ERR_NVS_NOT_INITIALIZED);
  ASSERT_EQ(nvs_flash_init(), ESP_OK);
  EXPECT_EQ(nvs_open("ns", NVS_READONLY, &h), ESP_ERR_NVS_NOT_FOUND);
}
