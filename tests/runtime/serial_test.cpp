#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#include "preflash/serial.h"

namespace fs = std::filesystem;
namespace serial = preflash::serial;

TEST(Base64, EncodesEachRemainder) {
  EXPECT_EQ(serial::base64("", 0), "");
  EXPECT_EQ(serial::base64("f", 1), "Zg==");
  EXPECT_EQ(serial::base64("fo", 2), "Zm8=");
  EXPECT_EQ(serial::base64("foo", 3), "Zm9v");
  EXPECT_EQ(serial::base64("\xff\x00\xfe", 3), "/wD+");
}

TEST(SerialRing, KeepsEverythingUntilFull) {
  serial::Ring ring(8);
  ring.append("abc", 3);
  ring.append("de", 2);
  EXPECT_EQ(ring.contents(), "abcde");
}

TEST(SerialRing, KeepsTheNewestBytes) {
  serial::Ring ring(8);
  ring.append("abcdef", 6);
  ring.append("ghijkl", 6);
  EXPECT_EQ(ring.contents(), "efghijkl");
}

TEST(SerialRing, TakesAWriteLargerThanItself) {
  serial::Ring ring(4);
  ring.append("abcdefghij", 10);
  EXPECT_EQ(ring.contents(), "ghij");
}

TEST(Outbox, GivesLinesInOrder) {
  serial::Outbox box(1024);
  box.push_message("{\"id\":1}");
  box.push_serial("hi", 2);
  std::string line;
  ASSERT_TRUE(box.pop(&line));
  EXPECT_EQ(line, "{\"id\":1}\n");
  box.sent();
  ASSERT_TRUE(box.pop(&line));
  EXPECT_EQ(line, "{\"event\":\"serial\",\"data\":\"aGk=\"}\n");
  box.sent();
}

// A client that stops reading loses its oldest output and hears how much, while
// replies are never dropped.
TEST(Outbox, DropsTheOldestSerialAndSaysHowMuch) {
  serial::Outbox box(4);
  box.push_serial("aa", 2);
  box.push_message("{\"id\":1}");
  box.push_serial("bb", 2);
  box.push_serial("cc", 2);
  std::string line;
  ASSERT_TRUE(box.pop(&line));
  EXPECT_EQ(line, "{\"event\":\"dropped\",\"bytes\":2}\n");
  box.sent();
  ASSERT_TRUE(box.pop(&line));
  EXPECT_EQ(line, "{\"id\":1}\n");
  box.sent();
  ASSERT_TRUE(box.pop(&line));
  EXPECT_EQ(line, "{\"event\":\"serial\",\"data\":\"YmI=\"}\n");
  box.sent();
  ASSERT_TRUE(box.pop(&line));
  EXPECT_EQ(line, "{\"event\":\"serial\",\"data\":\"Y2M=\"}\n");
  box.sent();
}

TEST(Outbox, FinishesWhatIsQueuedOnceClosed) {
  serial::Outbox box(1024);
  box.push_message("last");
  box.close();
  std::string line;
  ASSERT_TRUE(box.pop(&line));
  EXPECT_EQ(line, "last\n");
  box.sent();
  EXPECT_FALSE(box.pop(&line));
}

TEST(Outbox, CountsWhatIsWaitingToBeSent) {
  const long before = serial::unsent();
  {
    serial::Outbox box(4);
    box.push_message("one");
    box.push_serial("aaaa", 4);
    box.push_serial("bbbb", 4);  // takes the place of the first
    EXPECT_EQ(serial::unsent(), before + 2);
    std::string line;
    ASSERT_TRUE(box.pop(&line));  // the dropped notice
    box.sent();
    ASSERT_TRUE(box.pop(&line));
    box.sent();
    EXPECT_EQ(serial::unsent(), before + 1);
  }
  EXPECT_EQ(serial::unsent(), before);
}

TEST(Outbox, ALineBeingWrittenIsCountedOnceWhenAbandoned) {
  const long before = serial::unsent();
  {
    serial::Outbox box(1024);
    box.push_message("one");
    box.push_message("two");
    std::string line;
    ASSERT_TRUE(box.pop(&line));
    box.abandon();
    EXPECT_EQ(serial::unsent(), before + 1);  // still on its way out
    box.sent();
    EXPECT_EQ(serial::unsent(), before);
  }
  EXPECT_EQ(serial::unsent(), before);
}

TEST(Outbox, UrgentMessagesGoAheadOfWhatIsQueued) {
  serial::Outbox box(1024);
  box.push_serial("aa", 2);
  box.push_message("late");
  box.push_urgent("first");
  std::string line;
  ASSERT_TRUE(box.pop(&line));
  EXPECT_EQ(line, "first\n");
  box.sent();
  ASSERT_TRUE(box.pop(&line));
  EXPECT_EQ(line, "{\"event\":\"serial\",\"data\":\"YWE=\"}\n");
  box.sent();
}

TEST(Outbox, WakesAWaitingReaderOnClose) {
  serial::Outbox box(1024);
  std::thread closer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    box.close();
  });
  std::string line;
  EXPECT_FALSE(box.pop(&line));
  closer.join();
}

namespace {

// The pump swaps the process's own stdout, so it gets a process of its own.
// Quoted for the shell, because a checkout's path can hold anything.
std::string quoted(const std::string& text) {
  std::string out = "'";
  for (const char c : text) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  return out + "'";
}

std::string run_helper(const std::string& mode, int* status) {
  const fs::path state = fs::temp_directory_path() / ("preflash_serial_" + std::to_string(getpid()) + mode);
  fs::remove_all(state);
  fs::create_directories(state);
  const std::string command =
      "PREFLASH_STATE=" + quoted(state.string()) + " " + quoted(SERIAL_HELPER) + " " + mode + " 2>&1";
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    *status = -1;
    return "";
  }
  std::string output;
  char buffer[65536];
  size_t got;
  while ((got = std::fread(buffer, 1, sizeof buffer, pipe)) > 0) output.append(buffer, got);
  *status = pclose(pipe);
  fs::remove_all(state);
  return output;
}

}  // namespace

// The flood leaves the pipe full, so a process that left without draining it would
// lose the end of what it wrote.
TEST(SerialPump, LastMessageSurvivesAnUnsupportedCall) {
  int status = -1;
  const std::string output = run_helper("unsupported", &status);
  EXPECT_NE(status, 0);
  EXPECT_NE(output.find("end of flood"), std::string::npos) << output.size();
  EXPECT_NE(output.find("flash_op is not supported in the simulator: because"), std::string::npos)
      << output.substr(output.size() > 200 ? output.size() - 200 : 0);
}

TEST(SerialPump, LastMessageSurvivesQuit) {
  int status = -1;
  const std::string output = run_helper("quit", &status);
  EXPECT_EQ(status, 0);
  EXPECT_NE(output.find("end of flood, no newline"), std::string::npos) << output.size();
}

TEST(SerialPump, LastMessageSurvivesARawAbort) {
  int status = -1;
  const std::string output = run_helper("abort", &status);
  EXPECT_NE(status, 0);
  EXPECT_NE(output.find("end of flood"), std::string::npos) << output.size();
}

TEST(SerialPump, LastMessageSurvivesACrash) {
  int status = -1;
  const std::string output = run_helper("crash", &status);
  EXPECT_NE(status, 0);
  EXPECT_NE(output.find("end of flood"), std::string::npos) << output.size();
}

TEST(SerialPump, LastMessageSurvivesInterrupt) {
  int status = -1;
  const std::string output = run_helper("interrupt", &status);
  EXPECT_EQ(status, 0);
  EXPECT_NE(output.find("end of flood"), std::string::npos) << output.size();
}

TEST(SerialPump, RestartGivesTheNextRunTheRealDescriptors) {
  int status = -1;
  const std::string output = run_helper("restart", &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("end of flood"), std::string::npos) << output.size();
  EXPECT_NE(output.find("second run: stdout same, stderr same"), std::string::npos)
      << output.substr(output.size() > 200 ? output.size() - 200 : 0);
}

// A queued restart runs while quit waits for the queue, and must not cancel it.
TEST(SerialPump, ARestartDuringQuitStops) {
  int status = -1;
  const std::string output = run_helper("restart_during_quit", &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_EQ(output.find("second run"), std::string::npos) << output;
}

TEST(SerialPump, StdoutAndStderrAreOneStreamWithReplay) {
  int status = -1;
  const std::string output = run_helper("subscribe", &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("subscriber saw: before\nafter out\nafter err\n;"), std::string::npos)
      << output;
}

// Nothing reads the helper's stdout, so a pump that waited for it would stop the firmware.
TEST(SerialPump, AStuckTerminalLosesOutputAndFirmwareCarriesOn) {
  int status = -1;
  const std::string output = run_helper("stalled", &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("flood finished"), std::string::npos) << output;
  EXPECT_NE(output.find("notice seen"), std::string::npos) << output;
}

// fd 2 was open, so a failure to save it must not end up closing it.
TEST(SerialPump, OutOfDescriptorsLeavesStdoutAndStderrAlone) {
  int status = -1;
  const std::string output = run_helper("no_fds", &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("start refused"), std::string::npos) << output;
  EXPECT_NE(output.find("stderr still open"), std::string::npos) << output;
}

// As with preflash run | head -1: firmware outlives whatever read its stdout.
TEST(SerialPump, ARealStdoutThatGoesAwayLeavesFirmwareRunning) {
  int status = -1;
  const std::string output = run_helper("stdout_gone", &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_NE(output.find("survived"), std::string::npos) << output;
}

TEST(SerialPump, BinaryOutputArrivesWhole) {
  int status = -1;
  const std::string output = run_helper("binary", &status);
  EXPECT_EQ(status, 0) << output;
  EXPECT_EQ(output, std::string("\xff\x00\xfe\n", 4));
}
