#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <sstream>
#include <iostream>
#include <thread>
#include <string>
#include <vector>

#include "esp_console.h"
#include "preflash/control.h"

namespace {

std::vector<std::string> g_args;

int record(int argc, char** argv) {
  g_args.assign(argv, argv + argc);
  std::printf("recorded %d\n", argc);
  return 0;
}

}  // namespace

TEST(Console, RunsARegisteredCommandWithArgs) {
  esp_console_cmd_t cmd = {};
  cmd.command = "record";
  cmd.help = "records";
  cmd.func = record;
  ASSERT_EQ(esp_console_cmd_register(&cmd), ESP_OK);

  g_args.clear();
  testing::internal::CaptureStdout();
  preflash::dispatch_line("record one  \"two words\" 3");
  const std::string out = testing::internal::GetCapturedStdout();
  EXPECT_EQ(g_args, (std::vector<std::string>{"record", "one", "two words", "3"}));
  EXPECT_EQ(out, "recorded 4\n");
}

TEST(Console, QuotesAndBackslashesSplitLikeTheDevice) {
  esp_console_cmd_t cmd = {};
  cmd.command = "record";
  cmd.func = record;
  ASSERT_EQ(esp_console_cmd_register(&cmd), ESP_OK);
  testing::internal::CaptureStdout();
  preflash::dispatch_line("record a\\ b \"c d\" e\"f\"");
  testing::internal::GetCapturedStdout();
  EXPECT_EQ(g_args, (std::vector<std::string>{"record", "a b", "c d", "e\"f\""}));
}

// The device's splitter doesn't reject an unclosed quote: the word runs to the
// end of the line.
TEST(Console, UnclosedQuoteRunsToTheEndOfTheLine) {
  esp_console_cmd_t cmd = {};
  cmd.command = "record";
  cmd.func = record;
  ASSERT_EQ(esp_console_cmd_register(&cmd), ESP_OK);
  g_args.clear();
  testing::internal::CaptureStdout();
  preflash::dispatch_line("record \"two words");
  testing::internal::GetCapturedStdout();
  EXPECT_EQ(g_args, (std::vector<std::string>{"record", "two words"}));
}

TEST(Console, ReplCanNotBeDeleted) {
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  esp_console_dev_usb_serial_jtag_config_t dev = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ASSERT_EQ(esp_console_new_repl_usb_serial_jtag(&dev, &config, &repl), ESP_OK);
  EXPECT_DEATH(repl->del(repl), "esp_console_repl_del");
}

TEST(Console, HelpListsRegisteredCommands) {
  esp_console_cmd_t cmd = {};
  cmd.command = "listed";
  cmd.help = "shows up";
  cmd.func = record;
  ASSERT_EQ(esp_console_cmd_register(&cmd), ESP_OK);
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  esp_console_dev_usb_serial_jtag_config_t dev = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ASSERT_EQ(esp_console_new_repl_usb_serial_jtag(&dev, &config, &repl), ESP_OK);
  testing::internal::CaptureStdout();
  preflash::dispatch_line("help");
  const std::string out = testing::internal::GetCapturedStdout();
  EXPECT_NE(out.find("listed"), std::string::npos);
  EXPECT_NE(out.find("shows up"), std::string::npos);
}

TEST(Console, SimLinesGoToControlsNotCommands) {
  esp_console_cmd_t cmd = {};
  cmd.command = "touch";
  cmd.func = record;
  ASSERT_EQ(esp_console_cmd_register(&cmd), ESP_OK);

  std::vector<std::string> got;
  preflash::add_control("touch", [&](std::vector<std::string> args) { got = args; });
  g_args.clear();
  testing::internal::CaptureStdout();
  preflash::dispatch_line("@sim touch 1 2");
  testing::internal::GetCapturedStdout();
  EXPECT_EQ(got, (std::vector<std::string>{"1", "2"}));
  EXPECT_TRUE(g_args.empty());
}

TEST(Console, UnknownCommandSaysSo) {
  testing::internal::CaptureStdout();
  preflash::dispatch_line("nonesuch 1");
  EXPECT_NE(testing::internal::GetCapturedStdout().find("Unrecognized command: nonesuch"),
            std::string::npos);
}

TEST(Console, UnknownControlSaysSo) {
  testing::internal::CaptureStdout();
  preflash::dispatch_line("@sim nonesuch 1");
  EXPECT_NE(testing::internal::GetCapturedStdout().find("Unknown control: nonesuch"),
            std::string::npos);
}

namespace {

std::mutex g_order_mutex;
std::vector<std::string> g_order;

int note(int argc, char** argv) {
  std::lock_guard<std::mutex> lock(g_order_mutex);
  g_order.push_back(argc > 1 ? argv[1] : "");
  return 0;
}

size_t noted() {
  std::lock_guard<std::mutex> lock(g_order_mutex);
  return g_order.size();
}

// Stands in for stdin: reads wait for more text until close() is called.
class FedInput : public std::streambuf {
 public:
  void feed(const std::string& text) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      pending_ += text;
    }
    ready_.notify_all();
  }
  void close() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
    }
    ready_.notify_all();
  }

 protected:
  int_type underflow() override {
    std::unique_lock<std::mutex> lock(mutex_);
    ready_.wait(lock, [this] { return !pending_.empty() || closed_; });
    if (pending_.empty()) return traits_type::eof();
    current_ = pending_.front();
    pending_.erase(0, 1);
    setg(&current_, &current_, &current_ + 1);
    return traits_type::to_int_type(current_);
  }

 private:
  std::mutex mutex_;
  std::condition_variable ready_;
  std::string pending_;
  bool closed_ = false;
  char current_ = 0;
};

}  // namespace

// Runs last in this file: the reader and the REPL can each only start once.
// It covers the REPL task end to end: replaying what waited, then taking lines
// as they arrive.
TEST(Console, EarlyLinesKeepStdinOrderEvenForControls) {
  esp_console_cmd_t cmd = {};
  cmd.command = "note";
  cmd.func = note;
  ASSERT_EQ(esp_console_cmd_register(&cmd), ESP_OK);
  preflash::add_control("note", [](std::vector<std::string> args) {
    std::lock_guard<std::mutex> lock(g_order_mutex);
    g_order.push_back(args.empty() ? "" : args[0]);
  });

  static std::atomic<bool> quit_seen{false};
  preflash::add_control("quit", [](std::vector<std::string>) { quit_seen = true; });

  FedInput input;
  input.feed("note first\n@sim note second\nnote third\n@sim note fourth\n@sim quit\n");
  std::streambuf* original = std::cin.rdbuf(&input);
  ASSERT_TRUE(preflash::start_console_reader());
  // Nothing runs yet: no REPL has started to take the first line.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_EQ(noted(), 0u);
  // Quit gets through although ordinary lines are waiting and no REPL has started.
  for (int i = 0; i < 500 && !quit_seen; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_TRUE(quit_seen);

  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  esp_console_dev_usb_serial_jtag_config_t dev = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ASSERT_EQ(esp_console_new_repl_usb_serial_jtag(&dev, &config, &repl), ESP_OK);
  ASSERT_EQ(esp_console_start_repl(repl), ESP_OK);
  for (int i = 0; i < 200 && noted() < 4; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // A REPL starts once.
  EXPECT_EQ(esp_console_start_repl(repl), ESP_ERR_INVALID_STATE);
  // With the REPL open, a line runs as soon as it arrives.
  input.feed("note fifth\n");
  for (int i = 0; i < 200 && noted() < 5; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  input.close();
  // The reader may still be inside getline; leave the buffer alone until it has hit EOF.
  for (int i = 0; i < 500 && !preflash::console_reader_done(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(preflash::console_reader_done());
  std::cin.rdbuf(original);

  std::lock_guard<std::mutex> lock(g_order_mutex);
  EXPECT_EQ(g_order, (std::vector<std::string>{"first", "second", "third", "fourth", "fifth"}));
}
