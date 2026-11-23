#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace preflash::window {

// The window's end of the link to its app: a Unix socket server for one app at a time, carrying
// newline-delimited JSON (see winlink/protocol.h). Nothing here blocks.
class Link {
 public:
  Link() = default;
  ~Link();
  Link(const Link&) = delete;
  Link& operator=(const Link&) = delete;

  // Takes over a stale socket file at `path`. False, with *error set, if another window holds it
  // or the socket can't be made.
  bool listen(const std::string& path, std::string* error);
  // Accepts a waiting app, which replaces any it had, then reads whole lines and passes each
  // valid message on. Lines that aren't a message are dropped. on_connect runs once a new app
  // is in, before any of its messages, so that what it sends goes out first.
  void poll(std::function<void(const nlohmann::json&)> on_message,
            std::function<void()> on_connect = nullptr);
  // False if no app is connected, or its connection can't take more than it has backed up.
  bool send(const nlohmann::json& message);
  bool connected() const { return client_ >= 0; }
  // How many apps have connected so far. on_connect is where a caller greets each one.
  uint64_t connections() const { return connections_; }
  // Now while an app is connected, otherwise when the last one dropped, or when listening started
  // if none has connected.
  std::chrono::steady_clock::time_point last_connected() const;

 private:
  void drop_client();
  bool flush();

  int listener_ = -1;
  int lock_ = -1;
  int client_ = -1;
  std::string path_;
  std::string in_;
  std::string out_;
  uint64_t connections_ = 0;
  std::chrono::steady_clock::time_point last_connected_;
};

}  // namespace preflash::window
