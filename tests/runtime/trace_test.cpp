#include <gtest/gtest.h>

#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_http_client.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "preflash/board.h"
#include "preflash/bus.h"
#include "preflash/net.h"
#include "preflash/trace.h"

using nlohmann::json;
using preflash::trace::Record;
using preflash::trace::Stream;
namespace fs = std::filesystem;

namespace {

// Everything a subscriber heard, safe to read from the test's thread.
class Heard {
 public:
  Heard() {
    id_ = preflash::trace::subscribe([this](const Record& r) {
      std::lock_guard<std::mutex> lock(mutex_);
      rows_.push_back(r);
    });
  }
  ~Heard() { stop(); }

  void stop() {
    if (id_ >= 0) preflash::trace::unsubscribe(id_);
    id_ = -1;
  }

  std::vector<Record> rows() {
    std::lock_guard<std::mutex> lock(mutex_);
    return rows_;
  }

  // Waits for the newest row of the stream that says op. A subscriber hears the ring too,
  // so older tests' rows can be here.
  Record wait_for(Stream stream, const std::string& op, int address = -1) {
    for (int tries = 0; tries < 500; ++tries) {
      const std::vector<Record> all = rows();
      for (auto r = all.rbegin(); r != all.rend(); ++r) {
        if (r->stream == stream && op == r->op && (address < 0 || r->address == address)) return *r;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ADD_FAILURE() << "no " << op << " row arrived";
    return Record{};
  }

 private:
  std::mutex mutex_;
  std::vector<Record> rows_;
  int id_ = -1;
};

class Answers : public preflash::I2cDevice {
 public:
  bool write(std::span<const uint8_t>) override { return true; }
  bool read(std::span<uint8_t> out) override {
    for (size_t i = 0; i < out.size(); ++i) out[i] = static_cast<uint8_t>(0xa0 + i);
    return true;
  }
};

}  // namespace

TEST(Trace, StaysOffUntilAClientAsks) {
  EXPECT_FALSE(preflash::trace::on());
  {
    Heard heard;
    EXPECT_TRUE(preflash::trace::on());
  }
  EXPECT_FALSE(preflash::trace::on());
}

TEST(Trace, AnOffRingWritesNothing) {
  const uint64_t before = preflash::trace::written();
  gpio_set_level(GPIO_NUM_5, 1);
  EXPECT_EQ(preflash::trace::written(), before);
}

TEST(Trace, RecordsKeepTheirOrderAndTimes) {
  Heard heard;
  preflash::trace::note(Stream::Net, "wifi", "one", "first");
  preflash::trace::note(Stream::Net, "wifi", "two", "second");
  const Record second = heard.wait_for(Stream::Net, "two");
  const Record first = heard.wait_for(Stream::Net, "one");
  EXPECT_EQ(second.seq, first.seq + 1);
  EXPECT_LE(first.us, second.us);
  EXPECT_STREQ(first.data, "first");
  EXPECT_STREQ(first.subject, "wifi");
}

TEST(Trace, LongTextIsCutNotOverrun) {
  Heard heard;
  const std::string longer(1000, 'x');
  preflash::trace::note(Stream::Net, longer.c_str(), "long", longer);
  const Record r = heard.wait_for(Stream::Net, "long");
  EXPECT_EQ(std::string(r.subject).size(), sizeof r.subject - 1);
  EXPECT_EQ(std::string(r.data).size(), sizeof r.data - 1);
}

TEST(Trace, ALateSubscriberHearsWhatTheRingHolds) {
  Heard first;
  preflash::trace::note(Stream::Net, "wifi", "kept", "");
  first.wait_for(Stream::Net, "kept");
  Heard late;
  late.wait_for(Stream::Net, "kept");
}

TEST(Trace, ABurstFromManyThreadsNeverWaitsAndNeverTears) {
  Heard heard;
  const uint64_t before = preflash::trace::written();
  std::vector<std::thread> threads;
  for (int t = 0; t < 8; ++t) {
    threads.emplace_back([t] {
      const std::string text(100, static_cast<char>('a' + t));
      for (int i = 0; i < 50000; ++i) preflash::trace::note(Stream::Net, "burst", "burst", text);
    });
  }
  for (std::thread& t : threads) t.join();
  EXPECT_GE(preflash::trace::written() - before, 8u * 50000u - preflash::trace::collisions());
  preflash::trace::note(Stream::Net, "wifi", "end of burst", "");
  heard.wait_for(Stream::Net, "end of burst");
  for (const Record& r : heard.rows()) {
    if (std::string(r.op) != "burst") continue;
    for (const char* c = r.data; *c; ++c) ASSERT_EQ(*c, r.data[0]);
  }
}

TEST(Trace, SavingWritesTheRingAsJsonLines) {
  const fs::path path = fs::temp_directory_path() / ("pftrace" + std::to_string(getpid()) + ".jsonl");
  {
    Heard heard;
    preflash::trace::note(Stream::Net, "wifi", "to save", "data");
    heard.wait_for(Stream::Net, "to save");
  }
  // The ring outlasts the client that filled it.
  const size_t saved = preflash::trace::save(path.string());
  EXPECT_GT(saved, 0u);
  EXPECT_EQ(fs::status(path).permissions() & fs::perms::all,
            fs::perms::owner_read | fs::perms::owner_write);
  std::ifstream in(path);
  std::string line;
  size_t lines = 0;
  bool found = false;
  while (std::getline(in, line)) {
    ++lines;
    const json row = json::parse(line);
    found = found || row["op"] == "to save";
  }
  EXPECT_EQ(lines, saved);
  EXPECT_TRUE(found);
  fs::remove(path);
}

TEST(Trace, SavingSomewhereImpossibleSaysSo) {
  EXPECT_THROW(preflash::trace::save("/no/such/folder/trace.jsonl"), std::runtime_error);
}

TEST(Trace, AnI2cReadShowsItsRegisterAndBytes) {
  preflash::I2cBus& bus = preflash::i2c_bus(1);
  bus.add(0x52, std::make_unique<Answers>());
  Heard heard;
  const uint8_t reg = 0x04;
  uint8_t data[3];
  EXPECT_TRUE(bus.transfer(0x52, {&reg, 1}, data));
  const Record r = heard.wait_for(Stream::I2c, "read", 0x52);
  EXPECT_EQ(r.bus, 1);
  EXPECT_EQ(r.address, 0x52);
  EXPECT_STREQ(r.data, "04: a0 a1 a2");
  EXPECT_FALSE(r.injected);
  bus.take(0x52);
}

TEST(Trace, AnI2cWriteShowsWhatItWrote) {
  preflash::I2cBus& bus = preflash::i2c_bus(1);
  bus.add(0x52, std::make_unique<Answers>());
  Heard heard;
  const uint8_t bytes[] = {0x10, 0x01, 0x02};
  EXPECT_TRUE(bus.transfer(0x52, bytes, {}));
  EXPECT_STREQ(heard.wait_for(Stream::I2c, "write", 0x52).data, "10: 01 02");
  bus.take(0x52);
}

TEST(Trace, NothingAtTheAddressIsANack) {
  Heard heard;
  const uint8_t reg = 0;
  uint8_t data[1];
  EXPECT_FALSE(preflash::i2c_bus(1).transfer(0x53, {&reg, 1}, data));
  const Record r = heard.wait_for(Stream::I2c, "read", 0x53);
  EXPECT_STREQ(r.data, "NACK");
  EXPECT_FALSE(r.injected);
}

TEST(Trace, ADeviceIsNamedFromTheBoardByBusAndAddress) {
  preflash::Board board{};
  board.i2c = {{"pcf85063", 0x51, 0}, {"axs15231b", 0x3b, 1}};
  EXPECT_EQ(preflash::trace::device_name(board, 0, 0x51), "PCF85063");
  EXPECT_EQ(preflash::trace::device_name(board, 1, 0x3b), "AXS15231B");
  EXPECT_EQ(preflash::trace::device_name(board, 0, 0x3b), "");
}

TEST(Trace, GpioShowsLevelAndDirection) {
  Heard heard;
  gpio_set_direction(GPIO_NUM_6, GPIO_MODE_OUTPUT);
  gpio_set_level(GPIO_NUM_6, 1);
  const Record direction = heard.wait_for(Stream::Gpio, "direction");
  EXPECT_STREQ(direction.subject, "gpio6");
  EXPECT_STREQ(direction.data, "output");
  const Record level = heard.wait_for(Stream::Gpio, "level");
  EXPECT_STREQ(level.subject, "gpio6");
  EXPECT_STREQ(level.data, "high");
}

TEST(Trace, TasksShowCreateAndDelete) {
  Heard heard;
  ASSERT_EQ(xTaskCreate([](void*) { vTaskDelete(nullptr); }, "brief", 2048, nullptr, 1, nullptr),
            pdPASS);
  const Record created = heard.wait_for(Stream::Task, "create");
  EXPECT_STREQ(created.subject, "brief");
  EXPECT_STREQ(created.data, "stack 2048");
  EXPECT_STREQ(heard.wait_for(Stream::Task, "delete").subject, "brief");
}

TEST(Trace, NvsShowsSetsAndCommitsWithoutTheValues) {
  const fs::path dir = fs::temp_directory_path() / ("pfnvs" + std::to_string(getpid()));
  setenv("PREFLASH_STATE", dir.c_str(), 1);
  ASSERT_EQ(nvs_flash_init(), ESP_OK);
  nvs_handle_t handle;
  ASSERT_EQ(nvs_open("wifi", NVS_READWRITE, &handle), ESP_OK);
  Heard heard;
  ASSERT_EQ(nvs_set_blob(handle, "password", "secret", 6), ESP_OK);
  ASSERT_EQ(nvs_commit(handle), ESP_OK);
  const Record set = heard.wait_for(Stream::Nvs, "set");
  EXPECT_STREQ(set.subject, "wifi/password");
  EXPECT_STREQ(set.data, "6 bytes");
  const Record commit = heard.wait_for(Stream::Nvs, "commit");
  EXPECT_STREQ(commit.subject, "wifi");
  EXPECT_STREQ(commit.data, "1 key");
  nvs_close(handle);
  nvs_flash_deinit();
  fs::remove_all(dir);
}

TEST(Trace, OfflineHttpIsAnInjectedRow) {
  preflash::net_set_online(false);
  Heard heard;
  esp_http_client_config_t config = {};
  config.url = "https://example.invalid/path";
  esp_http_client_handle_t client = esp_http_client_init(&config);
  EXPECT_NE(esp_http_client_perform(client), ESP_OK);
  esp_http_client_cleanup(client);
  preflash::net_set_online(true);
  const Record r = heard.wait_for(Stream::Net, "GET");
  EXPECT_STREQ(r.subject, "http");
  EXPECT_STREQ(r.data, "https://example.invalid/path offline (injected)");
  EXPECT_TRUE(r.injected);
}
