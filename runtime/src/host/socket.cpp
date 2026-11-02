#include "preflash/socket.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "esp_timer.h"
#include "preflash/board.h"
#include "preflash/control.h"
#include "preflash/net.h"
#include "preflash/panel.h"
#include "preflash/serial.h"
#include "preflash/touch.h"

namespace preflash::socket {
namespace {

using nlohmann::json;

constexpr int kProtocolVersion = 1;
constexpr int kBacklog = 16;
constexpr size_t kReadChunk = 64 * 1024;
constexpr size_t kLongestRequest = 1024 * 1024;
// Room for the ring's replay and then some, past which a stuck client loses serial.
constexpr size_t kSerialQueueBytes = 1024 * 1024;
// A client asking for frames and reading none is cut off here: a reply is never dropped.
constexpr size_t kLongestReplyQueue = 64 * 1024 * 1024;
constexpr int kHolderAnswerSeconds = 1;

char g_path[sizeof(sockaddr_un::sun_path)] = "";
int g_listener = -1;

struct Connection {
  explicit Connection(int socket_fd) : fd(socket_fd), out(kSerialQueueBytes) {}
  ~Connection() { close(fd); }

  int fd;
  serial::Outbox out;
  std::optional<int> subscription;
};

struct Clients {
  std::mutex mutex;
  std::list<std::shared_ptr<Connection>> connections;
  bool restarting = false;
};

Clients& clients() {
  static Clients* c = new Clients;  // never destroyed: threads outlive static destructors
  return *c;
}

std::string encode(const json& message) {
  return message.dump(-1, ' ', false, json::error_handler_t::replace);
}

void reply_ok(Connection& c, const json& id, json fields = json::object()) {
  fields["id"] = id;
  fields["ok"] = true;
  c.out.push_message(encode(fields));
}

void reply_error(Connection& c, const json& id, const std::string& error) {
  c.out.push_message(encode({{"id", id}, {"ok", false}, {"error", error}}));
}

std::string project_name() {
  const char* project = std::getenv("PREFLASH_PROJECT");
  return project ? project : board().name;
}

std::optional<int> int_field(const json& request, const char* name) {
  const auto it = request.find(name);
  if (it == request.end() || !it->is_number_integer()) return std::nullopt;
  const auto value = it->get<int64_t>();
  if (value < INT32_MIN || value > INT32_MAX) return std::nullopt;
  return static_cast<int>(value);
}

std::optional<std::string> text_field(const json& request, const char* name) {
  const auto it = request.find(name);
  if (it == request.end() || !it->is_string()) return std::nullopt;
  return it->get<std::string>();
}

void op_hello(Connection& c, const json&, const json& id) {
  reply_ok(c, id,
           {{"protocol", kProtocolVersion},
            {"project", project_name()},
            {"board", board().name},
            {"pid", getpid()}});
}

void op_status(Connection& c, const json&, const json& id) {
  reply_ok(c, id,
           {{"project", project_name()},
            {"board", board().name},
            {"width", board().width},
            {"height", board().height},
            {"pid", getpid()},
            {"uptime_ms", esp_timer_get_time() / 1000},
            {"backlight", panel::brightness()},
            {"frames", panel::frames_drawn()},
            {"fps", panel::fps()},
            {"wifi", wifi_state()}});
}

void op_send(Connection& c, const json& request, const json& id) {
  const auto line = text_field(request, "line");
  if (!line) {
    reply_error(c, id, "send needs a line");
  } else if (line->find_first_of("\r\n") != std::string::npos) {
    reply_error(c, id, "send takes one line, with no newline in it");
  } else {
    // First, because "@sim quit" doesn't come back.
    reply_ok(c, id);
    submit_line(*line);
  }
}

void op_subscribe(Connection& c, const json& request, const json& id) {
  const auto streams = request.find("streams");
  if (streams == request.end() || !streams->is_array()) {
    reply_error(c, id, "subscribe needs a list of streams");
    return;
  }
  for (const json& stream : *streams) {
    if (stream != "serial") {
      reply_error(c, id, "unknown stream: " + encode(stream));
      return;
    }
  }
  reply_ok(c, id);
  if (streams->empty() || c.subscription) return;
  c.subscription = serial::subscribe([out = &c.out](const char* data, size_t size) {
    out->push_serial(data, size);
  });
}

void op_frame(Connection& c, const json&, const json& id) {
  const std::vector<uint8_t> frame = panel::copy_frame();
  reply_ok(c, id,
           {{"width", board().width},
            {"height", board().height},
            {"format", "rgb565le"},
            {"data", serial::base64(frame.data(), frame.size())}});
}

void op_touch(Connection& c, const json& request, const json& id) {
  const auto x = int_field(request, "x");
  const auto y = int_field(request, "y");
  const auto ms = request.contains("ms") ? int_field(request, "ms") : touch::kDefaultPressMs;
  if (!x || !y || !ms || !touch::press(*x, *y, *ms)) {
    reply_error(c, id, "touch needs x and y on the panel, and ms of 0 or more");
    return;
  }
  reply_ok(c, id);
}

void op_shot(Connection& c, const json& request, const json& id) {
  const auto path = text_field(request, "path");
  if (!path) {
    reply_error(c, id, "shot needs a path");
    return;
  }
  try {
    panel::save_bmp(path->c_str());
  } catch (const std::exception& e) {
    reply_error(c, id, e.what());
    return;
  }
  reply_ok(c, id);
}

void op_quit(Connection& c, const json&, const json& id) {
  reply_ok(c, id);
  finish_queued_lines();
  quit();
}

using Op = void (*)(Connection&, const json& request, const json& id);

const std::map<std::string, Op> kOps = {
    {"hello", op_hello}, {"status", op_status}, {"send", op_send}, {"subscribe", op_subscribe},
    {"frame", op_frame}, {"touch", op_touch},   {"shot", op_shot}, {"quit", op_quit},
};

void handle_line(Connection& c, const std::string& line) {
  const json request = json::parse(line, nullptr, false);
  if (!request.is_object()) {
    reply_error(c, nullptr, "a request is one JSON object per line");
    return;
  }
  const json id = request.value("id", json());
  const auto name = text_field(request, "op");
  if (!name) {
    reply_error(c, id, "a request needs an op");
    return;
  }
  const auto op = kOps.find(*name);
  if (op == kOps.end()) {
    reply_error(c, id, "unknown op: " + *name);
    return;
  }
  op->second(c, request, id);
}

void forget(const std::shared_ptr<Connection>& c) {
  std::lock_guard<std::mutex> lock(clients().mutex);
  clients().connections.remove(c);
}

// Runs requests in order, each on this thread, so that one client's long press holds up
// that client alone and nothing here waits on the console's queue.
void read_requests(std::shared_ptr<Connection> c) {
  std::string buffer;
  std::vector<char> chunk(kReadChunk);
  bool cut_off = false;
  while (!cut_off) {
    const ssize_t got = read(c->fd, chunk.data(), chunk.size());
    if (got == 0) break;
    if (got < 0) {
      if (errno == EINTR) continue;
      cut_off = true;
      break;
    }
    buffer.append(chunk.data(), static_cast<size_t>(got));
    size_t start = 0;
    for (size_t end; (end = buffer.find('\n', start)) != std::string::npos; start = end + 1) {
      handle_line(*c, buffer.substr(start, end - start));
      if (c->out.message_bytes() > kLongestReplyQueue) {
        cut_off = true;
        break;
      }
    }
    if (cut_off) break;
    buffer.erase(0, start);
    if (buffer.size() > kLongestRequest) {
      reply_error(*c, nullptr, "that request is too long");
      break;
    }
  }
  if (c->subscription) serial::unsubscribe(*c->subscription);
  forget(c);
  c->out.close();
  if (cut_off) {
    c->out.abandon();
    shutdown(c->fd, SHUT_RDWR);
  }
}

void write_lines(std::shared_ptr<Connection> c) {
  std::string line;
  while (c->out.pop(&line)) {
    const char* at = line.data();
    size_t left = line.size();
    while (left > 0) {
      const ssize_t wrote = write(c->fd, at, left);
      if (wrote < 0) {
        if (errno == EINTR) continue;
        c->out.abandon();
        shutdown(c->fd, SHUT_RDWR);
        return;
      }
      at += wrote;
      left -= static_cast<size_t>(wrote);
    }
    c->out.sent();
  }
  shutdown(c->fd, SHUT_RDWR);
}

// macOS raises SIGPIPE for a socket at the process, past any thread's mask, so a client
// hanging up mid-write would stop the app. This turns it into an error from write.
void ignore_hang_ups(int fd) {
  const int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
}

void accept_clients() {
  serial::mask_signals_on_this_thread();
  for (;;) {
    const int fd = accept(g_listener, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EBADF || errno == EINVAL) return;
      // Out of descriptors, or a client that gave up: either way, the next one may work.
      if (errno != EINTR && errno != ECONNABORTED) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      continue;
    }
    ignore_hang_ups(fd);
    auto connection = std::make_shared<Connection>(fd);
    {
      std::lock_guard<std::mutex> lock(clients().mutex);
      if (clients().restarting) continue;
      clients().connections.push_back(connection);
    }
    try {
      std::thread(write_lines, connection).detach();
      std::thread(read_requests, connection).detach();
    } catch (const std::system_error&) {
      connection->out.close();
      forget(connection);
    }
  }
}

sockaddr_un address_of(const char* path) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, path, sizeof addr.sun_path - 1);
  return addr;
}

// Who answers on the socket, if anyone. Whatever doesn't reply in time still holds it.
std::optional<std::string> holder(const char* path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return std::nullopt;
  const sockaddr_un addr = address_of(path);
  if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
    close(fd);
    return std::nullopt;
  }
  ignore_hang_ups(fd);
  std::string name = "another app";
  timeval wait = {kHolderAnswerSeconds, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &wait, sizeof wait);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &wait, sizeof wait);
  const std::string request = encode({{"id", 1}, {"op", "hello"}}) + "\n";
  std::string reply;
  char chunk[1024];
  if (write(fd, request.data(), request.size()) == static_cast<ssize_t>(request.size())) {
    ssize_t got;
    while (reply.find('\n') == std::string::npos && (got = read(fd, chunk, sizeof chunk)) > 0) {
      reply.append(chunk, static_cast<size_t>(got));
    }
  }
  close(fd);
  const json answer = json::parse(reply.substr(0, reply.find('\n')), nullptr, false);
  if (answer.is_object() && answer.contains("project")) {
    name = answer.value("project", name) + " (pid " + std::to_string(answer.value("pid", 0)) + ")";
  }
  return name;
}

}  // namespace

bool claim(std::string* error) {
  if (g_listener >= 0) {
    close(g_listener);
    g_listener = -1;
    g_path[0] = '\0';
  }
  const char* path = std::getenv("PREFLASH_SOCKET");
  if (!path || !*path) return true;
  const std::string shown(path);
  if (shown.size() >= sizeof g_path) {
    *error = "the socket's path is too long: " + shown;
    return false;
  }

  struct stat info {};
  if (lstat(path, &info) == 0) {
    if (!S_ISSOCK(info.st_mode)) {
      *error = shown + " is in the way of the socket, and isn't one";
      return false;
    }
    if (const auto name = holder(path)) {
      *error = *name + " already holds the socket at " + shown;
      return false;
    }
    unlink(path);
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = std::string("no socket: ") + std::strerror(errno);
    return false;
  }
  const sockaddr_un addr = address_of(path);
  // Only the user may connect. Nothing else runs yet, so the umask can't catch other files.
  const mode_t mask = umask(0177);
  const int bound = bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr);
  const int bind_errno = errno;
  umask(mask);
  if (bound != 0 || listen(fd, kBacklog) != 0) {
    *error = "couldn't listen at " + shown + ": " + std::strerror(bound != 0 ? bind_errno : errno);
    close(fd);
    return false;
  }
  g_listener = fd;
  std::strncpy(g_path, path, sizeof g_path - 1);
  return true;
}

bool serve() {
  if (g_listener < 0) return true;
  try {
    std::thread(accept_clients).detach();
  } catch (const std::system_error&) {
    return false;
  }
  return true;
}

void announce_restart() {
  std::lock_guard<std::mutex> lock(clients().mutex);
  clients().restarting = true;
  for (const auto& c : clients().connections) c->out.push_message(encode({{"event", "restarting"}}));
}

void leave(int code) {
  serial::release();
  if (g_path[0]) unlink(g_path);
  _exit(code);
}

void quit() {
  serial::flush_stdio();
  leave(0);
}

}  // namespace preflash::socket
