#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "host/window.h"
#include "preflash/board.h"
#include "preflash/hardware.h"
#include "preflash/touch.h"
#include "window/link.h"
#include "winlink/backlight.h"

namespace {

using preflash::window::Link;
using nlohmann::json;

class WindowLink : public ::testing::Test {
 protected:
  void SetUp() override {
    // A short path: a socket's is limited to about a hundred bytes.
    dir_ = std::filesystem::temp_directory_path() /
           ("pfwl" + std::to_string(::getpid()) + "-" + std::to_string(++counter));
    std::filesystem::create_directories(dir_);
    path_ = (dir_ / "w.sock").string();
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  int connect_client() {
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path_.c_str(), sizeof addr.sun_path - 1);
    EXPECT_EQ(connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
    return fd;
  }

  // Polls until `want` messages have arrived or a second has passed.
  std::vector<json> collect(Link& link, size_t want) {
    std::vector<json> got;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (got.size() < want && std::chrono::steady_clock::now() < deadline) {
      link.poll([&](const json& m) { got.push_back(m); });
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return got;
  }

  static inline int counter = 0;
  std::filesystem::path dir_;
  std::string path_;
};

void write_all(int fd, const std::string& s) { ASSERT_EQ(write(fd, s.data(), s.size()), static_cast<ssize_t>(s.size())); }

TEST_F(WindowLink, AnAppConnectsAndMessagesArriveWhole) {
  Link link;
  std::string error;
  ASSERT_TRUE(link.listen(path_, &error)) << error;
  EXPECT_FALSE(link.connected());
  const int app = connect_client();
  // Poll between the pieces, so that each poll reads only part of a line.
  std::vector<json> got;
  auto poll_a_while = [&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    for (int i = 0; i < 5; i++) link.poll([&](const json& m) { got.push_back(m); });
  };
  write_all(app, "{\"op\":\"sta");
  poll_a_while();
  EXPECT_TRUE(link.connected());
  EXPECT_EQ(got.size(), 0u) << "half a line isn't a message";
  write_all(app, "te\",\"backlight\":5}\n{\"op\":\"close\"}");
  poll_a_while();
  ASSERT_EQ(got.size(), 1u) << "the close has no newline yet";
  EXPECT_EQ(got[0]["op"], "state");
  EXPECT_EQ(got[0]["backlight"], 5);
  write_all(app, "\n");
  for (const json& m : collect(link, 1)) got.push_back(m);
  ASSERT_EQ(got.size(), 2u);
  EXPECT_EQ(got[1]["op"], "close");
  EXPECT_TRUE(link.connected());
  close(app);
}

TEST_F(WindowLink, ASecondWindowOnTheSamePathFails) {
  Link first, second;
  std::string error;
  ASSERT_TRUE(first.listen(path_, &error)) << error;
  EXPECT_FALSE(second.listen(path_, &error));
  EXPECT_NE(error.find("already"), std::string::npos) << error;
  // The first still serves.
  const int app = connect_client();
  write_all(app, "{\"op\":\"close\"}\n");
  EXPECT_EQ(collect(first, 1).size(), 1u);
  close(app);
}

TEST_F(WindowLink, AStaleSocketFileIsTakenOver) {
  const int stale = ::socket(AF_UNIX, SOCK_STREAM, 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path_.c_str(), sizeof addr.sun_path - 1);
  ASSERT_EQ(bind(stale, reinterpret_cast<sockaddr*>(&addr), sizeof addr), 0);
  close(stale);  // the file stays, with nobody behind it
  ASSERT_TRUE(std::filesystem::exists(path_));

  Link link;
  std::string error;
  ASSERT_TRUE(link.listen(path_, &error)) << error;
  const int app = connect_client();
  write_all(app, "{\"op\":\"close\"}\n");
  EXPECT_EQ(collect(link, 1).size(), 1u);
  close(app);
}

TEST_F(WindowLink, ANewAppReplacesTheOldConnection) {
  Link link;
  std::string error;
  ASSERT_TRUE(link.listen(path_, &error)) << error;
  const int first = connect_client();
  write_all(first, "{\"op\":\"board\",\"n\":1}\n");
  ASSERT_EQ(collect(link, 1).size(), 1u);
  const int second = connect_client();
  write_all(second, "{\"op\":\"board\",\"n\":2}\n");
  const auto got = collect(link, 1);
  ASSERT_EQ(got.size(), 1u);
  EXPECT_EQ(got[0]["n"], 2);
  EXPECT_EQ(link.connections(), 2u);

  // Sends go to the second, and the first has been closed on.
  ASSERT_TRUE(link.send({{"op", "touch"}, {"x", 1}}));
  char buf[64];
  const ssize_t n = read(second, buf, sizeof buf);
  ASSERT_GT(n, 0);
  EXPECT_NE(std::string(buf, static_cast<size_t>(n)).find("\"touch\""), std::string::npos);
  EXPECT_EQ(read(first, buf, sizeof buf), 0);
  close(first);
  close(second);
}

TEST_F(WindowLink, AnAppHangingUpEndsTheConnectionAfterItsLastLine) {
  Link link;
  std::string error;
  ASSERT_TRUE(link.listen(path_, &error)) << error;
  const int app = connect_client();
  write_all(app, "{\"op\":\"close\"}\n");
  close(app);
  EXPECT_EQ(collect(link, 1).size(), 1u);
  link.poll([](const json&) {});
  EXPECT_FALSE(link.connected());
  EXPECT_FALSE(link.send({{"op", "quit"}}));
}

TEST(WindowBacklight, FollowsASquareRootCurve) {
  EXPECT_EQ(preflash::winlink::backlight_level(0), 0);
  EXPECT_EQ(preflash::winlink::backlight_level(100), 255);
  EXPECT_EQ(preflash::winlink::backlight_level(25), 128);
}

}  // namespace

// The app's side of the link: the handler its reader thread runs for each message.
TEST(WindowInput, ATouchFromTheWindowClampsLikePointer) {
  while (preflash::touch::pop()) {
  }
  preflash::window::handle({{"op", "touch"}, {"x", -5}, {"y", 9999}, {"down", true}});
  const auto e = preflash::touch::pop();
  ASSERT_TRUE(e);
  EXPECT_EQ(e->x, 0);
  EXPECT_EQ(e->y, 7);  // the suite's board is 16 by 8
  EXPECT_TRUE(e->down);
  EXPECT_FALSE(preflash::touch::pop());
}

// Rotate steps the board's own rotation, a quarter each way, round past 0 and 270.
TEST(WindowInput, RotateStepsTheBoardsRotation) {
  preflash::hardware::load(preflash::Board{});
  preflash::window::handle({{"op", "rotate"}, {"by", 90}});
  EXPECT_EQ(preflash::hardware::rotation(), 90);
  preflash::window::handle({{"op", "rotate"}, {"by", -90}});
  preflash::window::handle({{"op", "rotate"}, {"by", -90}});
  EXPECT_EQ(preflash::hardware::rotation(), 270);
  preflash::window::handle({{"op", "rotate"}, {"by", 45}});  // not a quarter: ignored
  EXPECT_EQ(preflash::hardware::rotation(), 270);
  preflash::window::handle({{"op", "rotate"}, {"by", 90}});
  EXPECT_EQ(preflash::hardware::rotation(), 0);
}
