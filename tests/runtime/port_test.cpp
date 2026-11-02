#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "preflash/port.h"

namespace fs = std::filesystem;
namespace port = preflash::port;

namespace {

std::vector<std::string> split(port::LineSplitter& lines, const std::string& input,
                               size_t* dropped = nullptr) {
  std::vector<std::string> out;
  const size_t lost = lines.feed(input.data(), input.size(),
                                 [&](const std::string& line) { out.push_back(line); });
  if (dropped) *dropped = lost;
  return out;
}

using Lines = std::vector<std::string>;

std::string quoted(const std::string& text) { return "'" + text + "'"; }

struct TempFolder {
  TempFolder() {
    char name[] = "/tmp/preflash-port-XXXXXX";
    path = mkdtemp(name);
  }
  ~TempFolder() { fs::remove_all(path); }
  std::string path;
};

// The port's writer and reader run in a process of their own, which restarts in some modes.
std::string run_helper(const std::string& mode, const std::string& environment, int* status) {
  const std::string command = environment + " " + quoted(PORT_HELPER) + " " + mode + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    *status = -1;
    return "";
  }
  std::string output;
  char buffer[4096];
  size_t got;
  while ((got = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) output.append(buffer, got);
  *status = pclose(pipe);
  return output;
}

std::string with_port(const TempFolder& folder) {
  return "PREFLASH_PORT=" + quoted(folder.path + "/port.tty");
}

}  // namespace

TEST(LineSplitter, EndsALineAtEitherByte) {
  port::LineSplitter lines(64);
  EXPECT_EQ(split(lines, "one\ntwo\rthree\r\nfour"), (Lines{"one", "two", "three"}));
  EXPECT_EQ(split(lines, "\n"), (Lines{"four"}));
}

TEST(LineSplitter, TakesACarriageReturnAndNewlineSplitAcrossReads) {
  port::LineSplitter lines(64);
  EXPECT_EQ(split(lines, "one\r"), (Lines{"one"}));
  EXPECT_EQ(split(lines, "\ntwo\n"), (Lines{"two"}));
}

TEST(LineSplitter, KeepsEmptyLines) {
  port::LineSplitter lines(64);
  EXPECT_EQ(split(lines, "\n\n"), (Lines{"", ""}));
}

TEST(LineSplitter, PassesAnyByteThrough) {
  port::LineSplitter lines(64);
  const std::string raw("a\0\xff b", 5);
  EXPECT_EQ(split(lines, raw + "\n"), (Lines{raw}));
}

TEST(LineSplitter, DropsALineTooLongToKeepAndRecovers) {
  port::LineSplitter lines(4);
  size_t dropped = 0;
  EXPECT_EQ(split(lines, "abcd\nabcde", &dropped), (Lines{"abcd"}));
  EXPECT_EQ(dropped, 0u);
  EXPECT_EQ(split(lines, "fgh\nok\n", &dropped), (Lines{"ok"}));
  EXPECT_EQ(dropped, 1u);
}

TEST(Port, IsOffUnlessAskedFor) {
  int status = -1;
  const std::string output = run_helper("off", "", &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("no port"), std::string::npos) << output;
}

TEST(Port, IsARawPrivateTerminalBehindALink) {
  TempFolder folder;
  int status = -1;
  const std::string output = run_helper("settings", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("path=" + folder.path + "/port.tty"), std::string::npos) << output;
  EXPECT_NE(output.find("device=/dev/ttys"), std::string::npos) << output;
  EXPECT_NE(output.find("echo=no canonical=no output=raw speed=115200"), std::string::npos) << output;
  EXPECT_NE(output.find("mode=600"), std::string::npos) << output;
  EXPECT_NE(output.find("fds above stderr, closed on exec"), std::string::npos) << output;
}

TEST(Port, KeepsItsSettingsAfterTheLastClientCloses) {
  TempFolder folder;
  int status = -1;
  const std::string output = run_helper("reopen", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("echo=no canonical=no"), std::string::npos) << output;
}

TEST(Port, NobodyAttachedCostsTheCallerNothing) {
  TempFolder folder;
  int status = -1;
  const std::string output = run_helper("nobody", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("wrote without waiting"), std::string::npos) << output;
  EXPECT_NE(output.find("gave up on the queue quickly"), std::string::npos) << output;
}

TEST(Port, AReaderGetsABurstLargerThanTheTerminalHolds) {
  TempFolder folder;
  int status = -1;
  const std::string output = run_helper("burst", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("burst arrived whole"), std::string::npos) << output;
}

TEST(Port, ASlowReaderLosesTheOldest) {
  TempFolder folder;
  int status = -1;
  const std::string output = run_helper("slow", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("kept the newest"), std::string::npos) << output;
}

TEST(Port, LinesJoinTheConsoleQueue) {
  TempFolder folder;
  int status = -1;
  const std::string output = run_helper("input", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("control ran: one"), std::string::npos) << output;
  EXPECT_NE(output.find("control ran: two"), std::string::npos) << output;
}

TEST(Port, SurvivesARestart) {
  TempFolder folder;
  int status = -1;
  const std::string output = run_helper("restart", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("same device after restart"), std::string::npos) << output;
  EXPECT_NE(output.find("fds closed on exec"), std::string::npos) << output;
  EXPECT_NE(output.find("control ran: waited"), std::string::npos) << output;
}

TEST(Port, IgnoresDescriptorsItDidNotInherit) {
  TempFolder folder;
  int status = -1;
  const std::string output =
      run_helper("settings", with_port(folder) + " PREFLASH_PORT_FDS=0:1", &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("device=/dev/ttys"), std::string::npos) << output;
}

TEST(Port, MakesTheExtraLinkAndLeavesOthersFilesAlone) {
  TempFolder folder;
  const std::string link = folder.path + "/extra.tty";
  const std::string environment = with_port(folder) + " PREFLASH_PORT_LINK=" + quoted(link);
  int status = -1;
  std::string output = run_helper("links", environment, &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("extra points at the device"), std::string::npos) << output;
  EXPECT_NE(output.find("both gone"), std::string::npos) << output;

  FILE* file = std::fopen(link.c_str(), "w");
  ASSERT_NE(file, nullptr);
  std::fclose(file);
  output = run_helper("settings", environment, &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("isn't a link"), std::string::npos) << output;
  EXPECT_TRUE(fs::is_regular_file(link));
}

TEST(Port, OffersNoPortItCantServe) {
  TempFolder folder;
  int status = -1;
  const std::string output = run_helper("nothreads", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("no port"), std::string::npos) << output;
  EXPECT_FALSE(fs::is_symlink(folder.path + "/port.tty")) << output;
}

TEST(Port, OffersTheExtraLinkWhenItsOwnFails) {
  TempFolder folder;
  const std::string link = folder.path + "/extra.tty";
  int status = -1;
  const std::string output =
      run_helper("settings", "PREFLASH_PORT=" + quoted(folder.path + "/none/port.tty") +
                                 " PREFLASH_PORT_LINK=" + quoted(link), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("path=" + link), std::string::npos) << output;
}

TEST(Port, LeavesAFileAtItsTemporaryNameAlone) {
  TempFolder folder;
  const std::string next = folder.path + "/port.tty.next";
  FILE* file = std::fopen(next.c_str(), "w");
  ASSERT_NE(file, nullptr);
  std::fclose(file);
  int status = -1;
  const std::string output = run_helper("settings", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("couldn't link"), std::string::npos) << output;
  EXPECT_TRUE(fs::is_regular_file(next));
}

TEST(Port, KeepsItsOwnNoticesOutOfThePort) {
  TempFolder folder;
  int status = -1;
  const std::string output = run_helper("notice", with_port(folder), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("clients heard the notice"), std::string::npos) << output;
  EXPECT_NE(output.find("the port stayed clean"), std::string::npos) << output;
}

TEST(Port, FallsBackToTheDeviceWhenTheLinkFails) {
  TempFolder folder;
  int status = -1;
  const std::string output =
      run_helper("settings", "PREFLASH_PORT=" + quoted(folder.path + "/none/port.tty"), &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("couldn't link"), std::string::npos) << output;
  EXPECT_NE(output.find("path=/dev/ttys"), std::string::npos) << output;
}
