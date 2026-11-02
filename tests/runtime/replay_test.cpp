#include <gtest/gtest.h>

#include <memory>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>

#include "preflash/bus.h"
#include "preflash/chips.h"
#include "preflash/replay.h"

using nlohmann::json;
using preflash::i2c_bus;

namespace {

constexpr uint8_t kExpander = 0x20;
constexpr uint8_t kMotion = 0x6b;

json transaction(int bus, int address, const std::string& write, int read_length) {
  return {{"bus", bus}, {"addr", address}, {"write", write}, {"read_len", read_length}};
}

std::vector<json> replay_all(const std::vector<json>& transactions) {
  std::stringstream in;
  for (const json& one : transactions) in << one.dump() << "\n";
  std::stringstream out;
  preflash::replay(in, out);
  std::vector<json> answers;
  for (std::string line; std::getline(out, line);) answers.push_back(json::parse(line));
  return answers;
}

class ReplayTest : public ::testing::Test {
 protected:
  void SetUp() override {
    i2c_bus(0).add(kExpander, std::make_unique<preflash::Tca9554>());
    i2c_bus(0).add(kMotion, std::make_unique<preflash::Qmi8658>());
  }
  void TearDown() override {
    i2c_bus(0).take(kExpander);
    i2c_bus(0).take(kMotion);
  }
};

}  // namespace

TEST_F(ReplayTest, ReadsWhatTheModelAnswers) {
  const auto answers = replay_all({transaction(0, kExpander, "03", 1)});
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_TRUE(answers[0]["acked"].get<bool>());
  EXPECT_EQ(answers[0]["read"], "ff");
  EXPECT_FALSE(answers[0].contains("refused"));
}

TEST_F(ReplayTest, EachWriteReachesTheModelBeforeTheNextRead) {
  const auto answers = replay_all({transaction(0, kExpander, "02aa", 0), transaction(0, kExpander, "02", 1)});
  ASSERT_EQ(answers.size(), 2u);
  EXPECT_EQ(answers[0]["read"], "");
  EXPECT_EQ(answers[1]["read"], "aa");
}

TEST_F(ReplayTest, NothingAtTheAddressIsNotAcked) {
  const auto answers = replay_all({transaction(0, 0x11, "00", 1)});
  ASSERT_EQ(answers.size(), 1u);
  EXPECT_FALSE(answers[0]["acked"].get<bool>());
  EXPECT_EQ(answers[0]["read"], "");
}

TEST_F(ReplayTest, ARefusedRegisterIsAnAnswerAndTheReplayGoesOn) {
  const auto answers = replay_all({transaction(0, kMotion, "7f", 1), transaction(0, kMotion, "00", 1)});
  ASSERT_EQ(answers.size(), 2u);
  EXPECT_FALSE(answers[0]["acked"].get<bool>());
  EXPECT_NE(answers[0]["refused"].get<std::string>().find("qmi8658 register 0x7f"), std::string::npos);
  EXPECT_TRUE(answers[1]["acked"].get<bool>());
  EXPECT_EQ(answers[1]["read"], "05");
}

TEST_F(ReplayTest, AnUnfinishedTransactionIsAnError) {
  std::stringstream in("{\"bus\":0}\n");
  std::stringstream out;
  EXPECT_THROW(preflash::replay(in, out), std::exception);
}

TEST_F(ReplayTest, ARefusalOutsideAReplayStillStopsTheProcess) {
  replay_all({});
  const uint8_t frame[] = {0x7f, 0x00};
  EXPECT_DEATH(i2c_bus(0).transfer(kMotion, frame, {}), "qmi8658 register 0x7f");
}
