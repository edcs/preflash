#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_console.h"
#include "freertos/FreeRTOS.h"
#include "preflash/control.h"

namespace {

constexpr TickType_t kWait = pdMS_TO_TICKS(2000);

void install() {
  usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  config.tx_buffer_size = 4096;
  config.rx_buffer_size = 128;
  ASSERT_EQ(usb_serial_jtag_driver_install(&config), ESP_OK);
}

// Reads until the text has come or the wait has run out.
std::string read_text(size_t size) {
  std::string text;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (text.size() < size && std::chrono::steady_clock::now() < deadline) {
    char buffer[64];
    const int got = usb_serial_jtag_read_bytes(buffer, sizeof buffer, pdMS_TO_TICKS(100));
    text.append(buffer, got);
  }
  return text;
}

std::atomic<int> g_runs{0};

int count_run(int, char**) {
  ++g_runs;
  return 7;
}

void add_counter() {
  esp_console_cmd_t cmd = {};
  cmd.command = "count";
  cmd.func = count_run;
  ASSERT_EQ(esp_console_cmd_register(&cmd), ESP_OK);
}

}  // namespace

TEST(UsbSerial, InstallChecksItsBuffers) {
  usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  config.rx_buffer_size = 64;
  EXPECT_EQ(usb_serial_jtag_driver_install(&config), ESP_ERR_INVALID_ARG);
  config.rx_buffer_size = 128;
  config.tx_buffer_size = 0;
  EXPECT_EQ(usb_serial_jtag_driver_install(&config), ESP_ERR_INVALID_ARG);
  EXPECT_FALSE(usb_serial_jtag_is_driver_installed());
}

TEST(UsbSerial, InstallsOnce) {
  install();
  EXPECT_TRUE(usb_serial_jtag_is_driver_installed());
  usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  config.rx_buffer_size = 128;
  EXPECT_EQ(usb_serial_jtag_driver_install(&config), ESP_ERR_INVALID_STATE);
}

TEST(UsbSerial, AReadTimesOutWithNothing) {
  install();
  char byte;
  EXPECT_EQ(usb_serial_jtag_read_bytes(&byte, 1, 0), 0);
  EXPECT_EQ(usb_serial_jtag_read_bytes(&byte, 1, pdMS_TO_TICKS(5)), 0);
  EXPECT_EQ(usb_serial_jtag_read_bytes(&byte, 0, portMAX_DELAY), 0);
}

TEST(UsbSerial, AReadTakesOnlyWhatFits) {
  install();
  ASSERT_TRUE(preflash::start_console_reader());
  ASSERT_TRUE(preflash::submit_bytes("abcdef", 6));
  char buffer[4] = {};
  EXPECT_EQ(usb_serial_jtag_read_bytes(buffer, 4, kWait), 4);
  EXPECT_EQ(std::string(buffer, 4), "abcd");
  EXPECT_EQ(read_text(2), "ef");
}

TEST(UsbSerial, LinesFromTheQueueBecomeBytes) {
  install();
  ASSERT_TRUE(preflash::start_console_reader());
  ASSERT_TRUE(preflash::submit_line("hello there"));
  EXPECT_EQ(read_text(12), "hello there\n");
}

TEST(UsbSerial, ControlLinesFromTheQueueStayWithTheRuntime) {
  install();
  std::atomic<int> runs{0};
  preflash::add_control("usbcheck", [&](std::vector<std::string>) { ++runs; });
  ASSERT_TRUE(preflash::start_console_reader());
  ASSERT_TRUE(preflash::submit_line("@sim usbcheck"));
  ASSERT_TRUE(preflash::submit_line("after"));
  EXPECT_EQ(read_text(6), "after\n");
  EXPECT_EQ(runs, 1);
}

TEST(UsbSerial, RawBytesNeverRunAsControls) {
  install();
  std::atomic<int> runs{0};
  preflash::add_control("usbcheck", [&](std::vector<std::string>) { ++runs; });
  ASSERT_TRUE(preflash::start_console_reader());
  const std::string line = "@sim usbcheck\n";
  ASSERT_TRUE(preflash::submit_bytes(line.data(), line.size()));
  EXPECT_EQ(read_text(line.size()), line);
  EXPECT_EQ(runs, 0);
}

TEST(UsbSerial, ALineBiggerThanTheBufferWaitsForTheReader) {
  install();
  ASSERT_TRUE(preflash::start_console_reader());
  const std::string line(500, 'x');
  ASSERT_TRUE(preflash::submit_line(line));
  EXPECT_EQ(read_text(501), line + "\n");
}

TEST(UsbSerial, LinesSentBeforeTheDriverWaitForIt) {
  ASSERT_TRUE(preflash::start_console_reader());
  ASSERT_TRUE(preflash::submit_line("early"));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  install();
  EXPECT_EQ(read_text(6), "early\n");
}

TEST(UsbSerial, SaysItTakesTheSerialOnceInstalled) {
  EXPECT_FALSE(preflash::firmware_reads_serial());
  install();
  EXPECT_TRUE(preflash::firmware_reads_serial());
}

TEST(UsbSerial, WritesReachTheStream) {
  testing::internal::CaptureStdout();
  install();
  EXPECT_EQ(usb_serial_jtag_write_bytes("sent", 4, kWait), 4);
  EXPECT_EQ(testing::internal::GetCapturedStdout(), "sent");
}

TEST(UsbSerial, AWriteWithNoDriverSendsNothing) {
  EXPECT_EQ(usb_serial_jtag_write_bytes("sent", 4, kWait), 0);
}

TEST(UsbSerial, TheReplAndTheDriverCantBothHaveThePort) {
  install();
  const esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  const esp_console_dev_usb_serial_jtag_config_t device = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  esp_console_repl_t* repl = nullptr;
  EXPECT_EQ(esp_console_new_repl_usb_serial_jtag(&device, &config, &repl), ESP_ERR_INVALID_STATE);
}

TEST(UsbSerial, TheDriverWontInstallOverARepl) {
  const esp_console_repl_config_t config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  const esp_console_dev_usb_serial_jtag_config_t device = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  esp_console_repl_t* repl = nullptr;
  ASSERT_EQ(esp_console_new_repl_usb_serial_jtag(&device, &config, &repl), ESP_OK);
  usb_serial_jtag_driver_config_t driver = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  driver.rx_buffer_size = 128;
  EXPECT_EQ(usb_serial_jtag_driver_install(&driver), ESP_ERR_INVALID_STATE);
}

TEST(UsbSerialDeathTest, UninstallingNamesTheCall) {
  EXPECT_DEATH(usb_serial_jtag_driver_uninstall(), "usb_serial_jtag_driver_uninstall");
}

TEST(UsbSerialDeathTest, ReadingWithNoDriverNamesTheCall) {
  char byte;
  EXPECT_DEATH(usb_serial_jtag_read_bytes(&byte, 1, 0), "usb_serial_jtag_read_bytes");
}

TEST(UsbSerialVfs, LfNeedsNoTranslation) {
  usb_serial_jtag_vfs_use_driver();
  usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_LF);
  usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
}

TEST(UsbSerialVfsDeathTest, OtherLineEndingsNameTheCall) {
  EXPECT_DEATH(usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF),
               "usb_serial_jtag_vfs_set_tx_line_endings");
  EXPECT_DEATH(usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR),
               "usb_serial_jtag_vfs_set_rx_line_endings");
}

TEST(ConsoleRun, NeedsInit) {
  int result = 0;
  EXPECT_EQ(esp_console_run("count", &result), ESP_ERR_INVALID_STATE);
  esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
  EXPECT_EQ(esp_console_init(nullptr), ESP_ERR_INVALID_ARG);
  EXPECT_EQ(esp_console_init(&config), ESP_OK);
  EXPECT_EQ(esp_console_init(&config), ESP_ERR_INVALID_STATE);
}

TEST(ConsoleRun, RunsACommandAndGivesItsResult) {
  esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
  ASSERT_EQ(esp_console_init(&config), ESP_OK);
  add_counter();
  int result = 0;
  EXPECT_EQ(esp_console_run("count now", &result), ESP_OK);
  EXPECT_EQ(result, 7);
  EXPECT_EQ(g_runs, 1);
}

TEST(ConsoleRun, SaysWhenThereIsNoSuchCommand) {
  esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
  ASSERT_EQ(esp_console_init(&config), ESP_OK);
  int result = 0;
  EXPECT_EQ(esp_console_run("nothing", &result), ESP_ERR_NOT_FOUND);
  EXPECT_EQ(esp_console_run("   ", &result), ESP_ERR_INVALID_ARG);
}

TEST(ConsoleRun, RegistersHelp) {
  int result = -1;
  EXPECT_EQ(esp_console_register_help_command(), ESP_ERR_INVALID_STATE);
  esp_console_config_t config = ESP_CONSOLE_CONFIG_DEFAULT();
  ASSERT_EQ(esp_console_init(&config), ESP_OK);
  ASSERT_EQ(esp_console_register_help_command(), ESP_OK);
  add_counter();
  testing::internal::CaptureStdout();
  EXPECT_EQ(esp_console_run("help", &result), ESP_OK);
  EXPECT_EQ(result, 0);
  std::fflush(stdout);
  EXPECT_NE(testing::internal::GetCapturedStdout().find("count"), std::string::npos);
}
