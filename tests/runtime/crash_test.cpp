#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

class Crash : public testing::Test {
 protected:
  void SetUp() override {
    char name[] = "/tmp/preflash-crash-XXXXXX";
    ASSERT_NE(mkdtemp(name), nullptr);
    dir_ = name;
    setenv("PREFLASH_CRASH_FILE", (dir_ / "crash.json").c_str(), 1);
    unsetenv("PREFLASH_CRASH_HOLD");
    unsetenv("PREFLASH_SANITIZER_LOG");
  }

  void TearDown() override {
    unsetenv("PREFLASH_CRASH_FILE");
    unsetenv("PREFLASH_CRASH_HOLD");
    unsetenv("PREFLASH_SANITIZER_LOG");
    fs::remove_all(dir_);
  }

  // Returns the helper's wait status. Its stream is kept in output_.
  int run(const std::string& args) {
    const fs::path out = dir_ / "out.txt";
    const std::string command =
        "exec '" CRASH_HELPER "' " + args + " > '" + out.string() + "' 2>&1 < /dev/null";
    const int status = std::system(command.c_str());
    std::ifstream in(out);
    std::stringstream text;
    text << in.rdbuf();
    output_ = text.str();
    return status;
  }

  json report() {
    std::ifstream in(dir_ / "crash.json");
    return json::parse(in, nullptr, false);
  }

  fs::path dir_;
  std::string output_;
};

}  // namespace

TEST_F(Crash, ANullReadSaysSoAndNamesTheTask) {
  EXPECT_NE(run("read"), 0);
  const json r = report();
  ASSERT_TRUE(r.is_object()) << output_;
  EXPECT_EQ(r["signal"], "SIGSEGV");
  EXPECT_EQ(r["access"], "read");
  EXPECT_EQ(r["address"], "0x0");
  EXPECT_EQ(r["task"], "painter");
  EXPECT_EQ(r["pid"].is_number_integer(), true);
  EXPECT_GE(r["frames"].size(), 3u);
  EXPECT_TRUE(r["load_address"].is_string());
  EXPECT_EQ(r["uuid"].get<std::string>().size(), 36u);
  EXPECT_TRUE(fs::exists(r["executable"].get<std::string>()));
  EXPECT_FALSE(fs::exists(dir_ / "crash.json.next"));
  EXPECT_NE(output_.find("crashed on painter"), std::string::npos) << output_;
}

TEST_F(Crash, ANullWriteSaysSo) {
  EXPECT_NE(run("write"), 0);
  EXPECT_EQ(report()["access"], "write");
}

TEST_F(Crash, AnAbortCounts) {
  EXPECT_NE(run("abort"), 0);
  const json r = report();
  EXPECT_EQ(r["signal"], "SIGABRT");
  EXPECT_EQ(r["kind"], "");
}

TEST_F(Crash, AnUnsupportedCallCarriesItsMessage) {
  EXPECT_NE(run("unsupported"), 0);
  const json r = report();
  EXPECT_EQ(r["kind"], "unsupported");
  EXPECT_EQ(r["message"], "flash_op is not supported in the simulator: because");
  EXPECT_NE(output_.find("flash_op is not supported"), std::string::npos);
}

TEST_F(Crash, AMessageIsEscaped) {
  EXPECT_NE(run("quote"), 0);
  const json r = report();
  ASSERT_TRUE(r.is_object());
  EXPECT_EQ(r["message"], "say\"\\\n is not supported in the simulator: tab\there");
}

// As esp_system's panic code words it, so the console reads as a device's does.
TEST_F(Crash, AFailedCheckPrintsAsOnADeviceAndReports) {
  EXPECT_NE(run("check"), 0);
  const json r = report();
  EXPECT_EQ(r["kind"], "error_check");
  EXPECT_EQ(r["message"], "ESP_ERR_NO_MEM");
  EXPECT_NE(output_.find("ESP_ERROR_CHECK failed: esp_err_t 0x101 (ESP_ERR_NO_MEM) at 0x"),
            std::string::npos)
      << output_;
  EXPECT_NE(output_.find("file: \""), std::string::npos) << output_;
  EXPECT_NE(output_.find("func: crash_now"), std::string::npos) << output_;
  EXPECT_NE(output_.find("expression: ESP_ERR_NO_MEM"), std::string::npos) << output_;
}

TEST_F(Crash, SystemAbortCarriesItsDetails) {
  EXPECT_NE(run("system_abort"), 0);
  const json r = report();
  EXPECT_EQ(r["kind"], "system_abort");
  EXPECT_EQ(r["message"], "heap corrupt");
  EXPECT_NE(output_.find("heap corrupt"), std::string::npos);
}

TEST_F(Crash, AStackOverflowStillReportsFromTheAlternateStack) {
  EXPECT_NE(run("stack"), 0);
  const json r = report();
  ASSERT_TRUE(r.is_object()) << output_;
  EXPECT_EQ(r["task"], "painter");
  EXPECT_GE(r["frames"].size(), 8u);
}

TEST_F(Crash, ATrapCounts) {
  EXPECT_NE(run("illegal"), 0);
  ASSERT_TRUE(report().is_object()) << output_;
}

// The crashing thread holds stdout's lock, which a handler using stdio would wait for.
TEST_F(Crash, AHeldStdioLockCantStopTheReport) {
  EXPECT_NE(run("locked"), 0);
  ASSERT_TRUE(report().is_object()) << output_;
}

TEST_F(Crash, HoldingFreezesFirmwareAndLeavesTheProcessUp) {
  setenv("PREFLASH_CRASH_HOLD", "1", 1);
  const fs::path result = dir_ / "held.txt";
  const int status = run("read '" + result.string() + "'");
  ASSERT_TRUE(WIFEXITED(status)) << status << output_;
  EXPECT_EQ(WEXITSTATUS(status), 7);
  std::ifstream in(result);
  std::string word;
  long counted = -1;
  in >> word >> counted;
  EXPECT_EQ(counted, 0);
}

// Firmware's bad bitmap faults with the pixels' lock held, and the panel must still answer.
TEST_F(Crash, AFaultInsideTheDrawLeavesTheFrameReadable) {
  setenv("PREFLASH_CRASH_HOLD", "1", 1);
  const fs::path result = dir_ / "held.txt";
  const int status = run("draw '" + result.string() + "'");
  ASSERT_TRUE(WIFEXITED(status)) << status << output_;
  EXPECT_EQ(WEXITSTATUS(status), 7);
  std::ifstream in(result);
  std::stringstream text;
  text << in.rdbuf();
  EXPECT_NE(text.str().find("frame"), std::string::npos);
}

TEST_F(Crash, ASanitizersLogJoinsTheReportAndTheStream) {
  const fs::path log = dir_ / "sanitizer.log";
  setenv("PREFLASH_SANITIZER_LOG", log.c_str(), 1);
  // The log is named for the process, which this shell keeps through its exec.
  const std::string command = "echo 'ERROR: AddressSanitizer: \"bad\"' > '" + log.string() +
                              ".'$$; exec '" CRASH_HELPER "' abort > '" + (dir_ / "o").string() +
                              "' 2>&1 < /dev/null";
  EXPECT_NE(std::system(command.c_str()), 0);
  const json r = report();
  ASSERT_TRUE(r.is_object());
  EXPECT_EQ(r["sanitizer"], "ERROR: AddressSanitizer: \"bad\"\n");
  std::ifstream in(dir_ / "o");
  std::stringstream text;
  text << in.rdbuf();
  EXPECT_NE(text.str().find("ERROR: AddressSanitizer"), std::string::npos);
  EXPECT_FALSE(fs::exists(log.string() + "." + std::to_string(r["pid"].get<int>())));
}

TEST_F(Crash, NoFileConfiguredStillDrainsAndDies) {
  unsetenv("PREFLASH_CRASH_FILE");
  EXPECT_NE(run("unsupported"), 0);
  EXPECT_NE(output_.find("flash_op is not supported"), std::string::npos);
}
