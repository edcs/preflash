#include <gtest/gtest.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <thread>

#include "preflash/control.h"
#include "preflash/panel.h"
#include "preflash/socket.h"
#include "preflash/touch.h"

using nlohmann::json;
namespace fs = std::filesystem;

namespace {

std::string socket_path(const char* name) {
  // Short, because a socket's path has to fit in 104 bytes.
  return "/tmp/pf" + std::to_string(getpid()) + name + ".sock";
}

sockaddr_un address_of(const std::string& path) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);
  return addr;
}

int connect_to(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  const sockaddr_un addr = address_of(path);
  if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int listen_on(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  const sockaddr_un addr = address_of(path);
  EXPECT_EQ(bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr), 0);
  EXPECT_EQ(listen(fd, 4), 0);
  return fd;
}

class Client {
 public:
  explicit Client(const std::string& path) : fd_(connect_to(path)) {
    EXPECT_GE(fd_, 0);
    timeval wait = {5, 0};
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
    const int on = 1;
    setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
  }
  ~Client() {
    if (fd_ >= 0) close(fd_);
  }

  // Stops early if the server hangs up.
  void send_raw(const std::string& text) {
    size_t sent = 0;
    while (sent < text.size()) {
      const ssize_t wrote = write(fd_, text.data() + sent, text.size() - sent);
      if (wrote <= 0) return;
      sent += static_cast<size_t>(wrote);
    }
  }

  json read_line() {
    for (;;) {
      const size_t end = buffer_.find('\n');
      if (end != std::string::npos) {
        const std::string line = buffer_.substr(0, end);
        buffer_.erase(0, end + 1);
        return json::parse(line);
      }
      char chunk[65536];
      const ssize_t got = read(fd_, chunk, sizeof chunk);
      if (got <= 0) return json();
      buffer_.append(chunk, static_cast<size_t>(got));
    }
  }

  json call(json request) {
    request["id"] = ++id_;
    send_raw(request.dump() + "\n");
    json reply = read_line();
    EXPECT_EQ(reply["id"], id_);
    return reply;
  }

 private:
  int fd_;
  int id_ = 0;
  std::string buffer_;
};

}  // namespace

// Runs before the suite below, whose server keeps the socket for the rest of the process.
TEST(SocketClaim, NeedsNoSocketWithoutTheVariable) {
  unsetenv("PREFLASH_SOCKET");
  std::string error;
  EXPECT_TRUE(preflash::socket::claim(&error)) << error;
}

TEST(SocketClaim, ClearsAStaleSocketAndKeepsItPrivate) {
  const std::string path = socket_path("stale");
  close(listen_on(path));
  ASSERT_TRUE(fs::exists(path));
  setenv("PREFLASH_SOCKET", path.c_str(), 1);

  std::string error;
  EXPECT_TRUE(preflash::socket::claim(&error)) << error;
  struct stat info {};
  ASSERT_EQ(stat(path.c_str(), &info), 0);
  EXPECT_TRUE(S_ISSOCK(info.st_mode));
  EXPECT_EQ(info.st_mode & 0777, 0600u);
  const int fd = connect_to(path);
  EXPECT_GE(fd, 0);
  close(fd);
  fs::remove(path);
}

TEST(SocketClaim, RefusesASocketALiveAppAnswersOn) {
  const std::string path = socket_path("live");
  const int server = listen_on(path);
  std::thread live([server] {
    const int client = accept(server, nullptr, nullptr);
    char buffer[256];
    ASSERT_GT(read(client, buffer, sizeof buffer), 0);
    const std::string reply = R"({"id":1,"ok":true,"project":"other-project","pid":4242})"
                              "\n";
    write(client, reply.data(), reply.size());
    close(client);
  });
  setenv("PREFLASH_SOCKET", path.c_str(), 1);

  std::string error;
  EXPECT_FALSE(preflash::socket::claim(&error));
  live.join();
  EXPECT_NE(error.find("other-project"), std::string::npos) << error;
  EXPECT_NE(error.find("4242"), std::string::npos) << error;
  EXPECT_TRUE(fs::exists(path));
  close(server);
  fs::remove(path);
}

TEST(SocketClaim, LeavesAFileThatIsNotASocket) {
  const std::string path = socket_path("file");
  std::ofstream(path) << "keep me";
  setenv("PREFLASH_SOCKET", path.c_str(), 1);

  std::string error;
  EXPECT_FALSE(preflash::socket::claim(&error));
  EXPECT_NE(error.find(path), std::string::npos) << error;
  EXPECT_TRUE(fs::exists(path));
  fs::remove(path);
}

TEST(SocketClaim, SaysWhenThePathIsTooLong) {
  const std::string path = "/tmp/" + std::string(200, 'x');
  setenv("PREFLASH_SOCKET", path.c_str(), 1);
  std::string error;
  EXPECT_FALSE(preflash::socket::claim(&error));
  EXPECT_NE(error.find("too long"), std::string::npos) << error;
}

class SocketServe : public testing::Test {
 protected:
  static void SetUpTestSuite() {
    path_ = new std::string(socket_path("serve"));
    setenv("PREFLASH_SOCKET", path_->c_str(), 1);
    setenv("PREFLASH_PROJECT", "demo", 1);
    std::string error;
    ASSERT_TRUE(preflash::socket::claim(&error)) << error;
    ASSERT_TRUE(preflash::socket::serve());
  }
  static void TearDownTestSuite() { fs::remove(*path_); }

  static const std::string& path() { return *path_; }

 private:
  static inline std::string* path_ = nullptr;
};

TEST_F(SocketServe, HelloGivesTheProtocolAndProject) {
  Client client(path());
  const json reply = client.call({{"op", "hello"}});
  EXPECT_EQ(reply["ok"], true);
  EXPECT_EQ(reply["protocol"], 1);
  EXPECT_EQ(reply["project"], "demo");
  EXPECT_EQ(reply["board"], "Test");
  EXPECT_EQ(reply["pid"], getpid());
}

TEST_F(SocketServe, StatusDescribesTheRunningApp) {
  Client client(path());
  const json reply = client.call({{"op", "status"}});
  EXPECT_EQ(reply["ok"], true);
  EXPECT_EQ(reply["project"], "demo");
  EXPECT_EQ(reply["board"], "Test");
  EXPECT_EQ(reply["width"], 16);
  EXPECT_EQ(reply["height"], 8);
  EXPECT_EQ(reply["pid"], getpid());
  EXPECT_TRUE(reply["uptime_ms"].is_number_integer());
  EXPECT_TRUE(reply["backlight"].is_number_integer());
  EXPECT_TRUE(reply["frames"].is_number_integer());
  EXPECT_TRUE(reply["fps"].is_number_integer());
  EXPECT_EQ(reply["wifi"], "off");
}

TEST_F(SocketServe, AnUnknownOpNamesTheOp) {
  Client client(path());
  const json reply = client.call({{"op", "nonesuch"}});
  EXPECT_EQ(reply["ok"], false);
  EXPECT_NE(reply["error"].get<std::string>().find("nonesuch"), std::string::npos);
}

TEST_F(SocketServe, ARequestWithNoOpIsAnError) {
  Client client(path());
  const json reply = client.call(json::object());
  EXPECT_EQ(reply["ok"], false);
}

TEST_F(SocketServe, BadJsonGetsAnErrorAndTheConnectionCarriesOn) {
  Client client(path());
  client.send_raw("this isn't json\n");
  const json reply = client.read_line();
  EXPECT_EQ(reply["ok"], false);
  EXPECT_TRUE(reply["id"].is_null());
  EXPECT_EQ(client.call({{"op", "hello"}})["ok"], true);
}

TEST_F(SocketServe, ALineTooLongToBeARequestEndsTheConnection) {
  Client client(path());
  client.send_raw(std::string(2 * 1024 * 1024, 'x'));
  EXPECT_EQ(client.read_line()["ok"], false);
  EXPECT_TRUE(client.read_line().is_null());
}

TEST_F(SocketServe, SendReachesTheConsoleQueue) {
  static std::atomic<int> seen{0};
  preflash::add_control("socket_note", [](std::vector<std::string> args) {
    seen = args.size() == 1 && args[0] == "hello" ? 1 : 2;
  });
  ASSERT_TRUE(preflash::start_console_reader());
  Client client(path());
  EXPECT_EQ(client.call({{"op", "send"}, {"line", "@sim socket_note hello"}})["ok"], true);
  for (int i = 0; i < 500 && seen == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  EXPECT_EQ(seen, 1);
}

TEST_F(SocketServe, SendNeedsOneLine) {
  Client client(path());
  EXPECT_EQ(client.call({{"op", "send"}})["ok"], false);
  EXPECT_EQ(client.call({{"op", "send"}, {"line", 7}})["ok"], false);
  EXPECT_EQ(client.call({{"op", "send"}, {"line", "one\ntwo"}})["ok"], false);
}

TEST_F(SocketServe, TouchPressesAndReleases) {
  while (preflash::touch::pop()) {
  }
  Client client(path());
  EXPECT_EQ(client.call({{"op", "touch"}, {"x", 3}, {"y", 4}, {"ms", 0}})["ok"], true);
  const auto down = preflash::touch::pop();
  const auto up = preflash::touch::pop();
  ASSERT_TRUE(down && up);
  EXPECT_EQ(down->x, 3);
  EXPECT_EQ(down->y, 4);
  EXPECT_TRUE(down->down);
  EXPECT_FALSE(up->down);
}

TEST_F(SocketServe, TouchMustLandOnThePanel) {
  Client client(path());
  EXPECT_EQ(client.call({{"op", "touch"}, {"x", 16}, {"y", 0}})["ok"], false);
  EXPECT_EQ(client.call({{"op", "touch"}, {"x", 0}, {"y", -1}})["ok"], false);
  EXPECT_EQ(client.call({{"op", "touch"}, {"x", 0}, {"y", 0}, {"ms", -1}})["ok"], false);
  EXPECT_EQ(client.call({{"op", "touch"}, {"x", "0"}, {"y", 0}})["ok"], false);
  EXPECT_EQ(client.call({{"op", "touch"}, {"x", 0.5}, {"y", 0}})["ok"], false);
  EXPECT_FALSE(preflash::touch::pop());
}

TEST_F(SocketServe, FrameGivesThePanelAndLeavesTheWindowsCopyAlone) {
  const uint16_t pixels[2] = {0xf800, 0x07e0};
  preflash::panel::draw(0, 0, 2, 1, pixels);
  std::vector<uint8_t> seen;
  ASSERT_TRUE(preflash::panel::take_frame(&seen));
  preflash::panel::draw(0, 0, 2, 1, pixels);

  Client client(path());
  const json reply = client.call({{"op", "frame"}});
  EXPECT_EQ(reply["ok"], true);
  EXPECT_EQ(reply["width"], 16);
  EXPECT_EQ(reply["height"], 8);
  EXPECT_EQ(reply["format"], "rgb565le");
  const std::string data = reply["data"];
  EXPECT_EQ(data.size(), (16 * 8 * 2 + 2) / 3 * 4u);
  EXPECT_EQ(data.substr(0, 4), "APjg");  // the first pixel and half of the second
  EXPECT_TRUE(preflash::panel::take_frame(&seen));
}

TEST_F(SocketServe, ShotWritesAFileOrSaysWhyNot) {
  const std::string file = socket_path("shot") + ".bmp";
  Client client(path());
  EXPECT_EQ(client.call({{"op", "shot"}, {"path", file}})["ok"], true);
  EXPECT_TRUE(fs::exists(file));
  fs::remove(file);
  const json failed = client.call({{"op", "shot"}, {"path", "/nonexistent/folder/shot.bmp"}});
  EXPECT_EQ(failed["ok"], false);
  EXPECT_NE(failed["error"].get<std::string>().find("/nonexistent/folder"), std::string::npos);
  EXPECT_EQ(client.call({{"op", "shot"}})["ok"], false);
}

TEST_F(SocketServe, SubscribeKnowsOnlyItsStreams) {
  Client client(path());
  EXPECT_EQ(client.call({{"op", "subscribe"}})["ok"], false);
  const json reply = client.call({{"op", "subscribe"}, {"streams", {"nonesuch"}}});
  EXPECT_EQ(reply["ok"], false);
  EXPECT_NE(reply["error"].get<std::string>().find("nonesuch"), std::string::npos);
  EXPECT_EQ(client.call({{"op", "subscribe"}, {"streams", {"serial"}}})["ok"], true);
  // Asking twice doesn't replay twice.
  EXPECT_EQ(client.call({{"op", "subscribe"}, {"streams", {"serial"}}})["ok"], true);
}

TEST_F(SocketServe, ClientsComeAndGo) {
  for (int i = 0; i < 20; ++i) {
    Client client(path());
    EXPECT_EQ(client.call({{"op", "hello"}})["ok"], true);
  }
  Client one(path());
  Client two(path());
  EXPECT_EQ(one.call({{"op", "hello"}})["ok"], true);
  EXPECT_EQ(two.call({{"op", "hello"}})["ok"], true);
  EXPECT_EQ(one.call({{"op", "status"}})["ok"], true);
}

// A half-closed client still gets its reply, so that a one-shot from nc works.
TEST_F(SocketServe, ARequestThenHangingUpOneWayStillGetsItsReply) {
  const int fd = connect_to(path());
  ASSERT_GE(fd, 0);
  const std::string request = R"({"id":9,"op":"hello"})"
                              "\n";
  ASSERT_EQ(write(fd, request.data(), request.size()), static_cast<ssize_t>(request.size()));
  shutdown(fd, SHUT_WR);
  std::string text;
  char chunk[1024];
  ssize_t got;
  while ((got = read(fd, chunk, sizeof chunk)) > 0) text.append(chunk, static_cast<size_t>(got));
  close(fd);
  EXPECT_EQ(json::parse(text)["id"], 9);
}
