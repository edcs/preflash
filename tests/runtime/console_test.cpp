#include <gtest/gtest.h>

#include <cstdio>
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
