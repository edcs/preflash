#include <crt_externs.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>

#include <nlohmann/json.hpp>

#include "preflash/board.h"
#include "preflash/crash.h"
#include "preflash/panel.h"
#include "preflash/socket.h"
#include "preflash/touch.h"
#include "../winlink/protocol.h"
#include "sdl_audio.h"
#include "window.h"

namespace preflash::window {
namespace {

using nlohmann::json;
using Clock = std::chrono::steady_clock;

// How long a window just started gets to answer.
constexpr auto kSpawnPatience = std::chrono::seconds(2);
constexpr auto kDialPause = std::chrono::milliseconds(20);
// A window that goes within this long of connecting didn't open, and after this many of those
// in a row there's no window to be had.
constexpr auto kQuickFailure = std::chrono::seconds(2);
constexpr int kFailuresBeforeGivingUp = 3;
// How often the main thread looks for something to tell the window.
constexpr auto kTurn = std::chrono::milliseconds(5);
// A window that takes no more than this lets the main thread go on, and is dropped.
constexpr int kSendPatienceSeconds = 1;
constexpr size_t kLongestLine = 1 << 20;

// The connection, for close_for_good to write to from any thread or a signal handler.
std::atomic<int> g_fd{-1};
// Set by the reader as it finishes with a connection, which the main thread then closes.
std::atomic<bool> g_dropped{false};
std::atomic<bool> g_closed{false};
std::atomic<bool> g_supervised{false};

std::string g_socket;
std::string g_exe;
std::string g_frame_path;
Clock::time_point g_connected_at;
int g_quick_failures = 0;

struct Shown {
  int backlight = -1;
  int rotation = -1;
} g_shown;

std::string from_env(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return value && *value ? value : fallback;
}

// The folder the app runs from, which is its build folder, for a run with nothing set, as
// when a script builds a project with cmake and runs the app itself.
std::string app_dir() {
  char path[PATH_MAX];
  uint32_t size = sizeof path;
  if (_NSGetExecutablePath(path, &size) != 0) return ".";
  const std::string exe(path);
  const size_t slash = exe.rfind('/');
  return slash == std::string::npos ? "." : exe.substr(0, slash);
}

std::string log_path() { return g_socket + ".log"; }

// What the window last said on its way out, if it said anything.
std::string last_words() {
  std::ifstream in(log_path());
  std::string line, last;
  while (std::getline(in, line)) {
    if (!line.empty()) last = line;
  }
  return last;
}

[[noreturn]] void give_up() {
  const std::string why = last_words();
  std::fprintf(stderr, "preflash: couldn't open the window%s%s\n", why.empty() ? "" : ": ",
               why.c_str());
  socket::leave(1);
}

int dial() {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (g_socket.size() >= sizeof addr.sun_path) return -1;
  std::strncpy(addr.sun_path, g_socket.c_str(), sizeof addr.sun_path - 1);
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) != 0) {
    close(fd);
    return -1;
  }
  const int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
  const timeval patience{kSendPatienceSeconds, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof patience);
  return fd;
}

// Starts a window in a session of its own, so that it outlives this process and nothing sent
// to the terminal's group reaches it. It holds none of this process's descriptors, so the
// serial stream ends when the app does, and what it says goes to a log beside its socket.
bool spawn() {
  posix_spawnattr_t attr;
  posix_spawn_file_actions_t actions;
  if (posix_spawnattr_init(&attr) != 0) return false;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    posix_spawnattr_destroy(&attr);
    return false;
  }
  sigset_t none;
  sigset_t all;
  sigemptyset(&none);
  sigfillset(&all);
  int err = posix_spawnattr_setsigmask(&attr, &none);
  if (!err) err = posix_spawnattr_setsigdefault(&attr, &all);
  if (!err) {
    err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID | POSIX_SPAWN_CLOEXEC_DEFAULT |
                                              POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  }
  const std::string log = log_path();
  if (!err) err = posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
  if (!err) err = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
  if (!err) {
    err = posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, log.c_str(),
                                           O_WRONLY | O_CREAT | O_TRUNC, 0600);
  }
  pid_t pid;
  char* const argv[] = {const_cast<char*>(g_exe.c_str()), const_cast<char*>(g_socket.c_str()),
                        nullptr};
  if (!err) err = posix_spawn(&pid, g_exe.c_str(), &actions, &attr, argv, *_NSGetEnviron());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  if (err) {
    std::ofstream(log) << "preflash-window: couldn't start " << g_exe << ": " << std::strerror(err)
                       << "\n";
  }
  return !err;
}

// Whole lines, or false if the window has gone or stopped reading.
bool send(const json& message) {
  const int fd = g_fd;
  if (fd < 0) return false;
  const std::string line = winlink::encode(message);
  size_t sent = 0;
  while (sent < line.size()) {
    const ssize_t n = write(fd, line.data() + sent, line.size() - sent);
    if (n > 0) {
      sent += static_cast<size_t>(n);
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else {
      // The reader hears it end, and the main thread starts again from there.
      shutdown(fd, SHUT_RDWR);
      return false;
    }
  }
  return true;
}

std::string project_name() { return from_env("PREFLASH_PROJECT", board().name); }

void send_board() {
  send({{"op", "board"},
        {"project", project_name()},
        {"board", board().name},
        {"width", board().width},
        {"height", board().height},
        {"buttons", json::array()},
        {"frame_path", g_frame_path}});
}

void send_state(bool always) {
  const Shown now{panel::brightness(), panel::rotation()};
  if (!always && now.backlight == g_shown.backlight && now.rotation == g_shown.rotation) return;
  if (send({{"op", "state"},
            {"power", "on"},
            {"backlight", now.backlight},
            {"rotation", now.rotation},
            {"held", json::array()}})) {
    g_shown = now;
  }
}

int as_int(const json& m, const char* key) {
  const auto it = m.find(key);
  return it != m.end() && it->is_number() ? it->get<int>() : 0;
}

// The window's messages, on a thread of its own, for one connection. A press the window
// started and this connection never ended is let go, so that firmware isn't left held down.
void read_link(int fd) {
  std::string in;
  bool pressed = false;
  touch::Event last{0, 0, false};
  char chunk[4096];
  for (;;) {
    const ssize_t got = read(fd, chunk, sizeof chunk);
    if (got < 0 && errno == EINTR) continue;
    if (got <= 0) break;
    in.append(chunk, static_cast<size_t>(got));
    size_t start = 0;
    for (size_t nl; (nl = in.find('\n', start)) != std::string::npos; start = nl + 1) {
      const auto message = winlink::decode_line(in.substr(start, nl - start));
      if (!message) continue;
      const std::string op = (*message)["op"].get<std::string>();
      if (op == "touch") {
        const auto down = message->find("down");
        last = {std::clamp(as_int(*message, "x"), 0, board().width - 1),
                std::clamp(as_int(*message, "y"), 0, board().height - 1),
                down != message->end() && down->is_boolean() && down->get<bool>()};
        pressed = last.down;
        if (!crash::happened()) touch::push(last);  // nothing will read it otherwise
      } else if (op == "quit") {
        socket::quit();
      } else if (op == "hello") {
        // Which buttons the window holds, of which there are none until boards have buttons.
        const auto held = message->find("held");
        if (held != message->end() && held->is_array() && !held->empty()) {
          std::fprintf(stderr, "preflash: the window holds buttons, which this app can't press\n");
        }
      } else if (op == "button" || op == "rotate") {
        std::fprintf(stderr, "preflash: the window sent %s, which this app can't do yet\n",
                     op.c_str());
      }
    }
    in.erase(0, start);
    if (in.size() > kLongestLine) in.clear();
  }
  if (pressed && !crash::happened()) touch::push({last.x, last.y, false});
  g_dropped = true;
}

// Connects to the window, starting one if nothing answers, and tells it the board.
bool connect_window() {
  int fd = dial();
  if (fd < 0) {
    if (!spawn()) return false;
    const auto deadline = Clock::now() + kSpawnPatience;
    while ((fd = dial()) < 0 && Clock::now() < deadline) std::this_thread::sleep_for(kDialPause);
    if (fd < 0) return false;
  }
  g_dropped = false;
  g_fd = fd;
  g_connected_at = Clock::now();
  try {
    std::thread(read_link, fd).detach();
  } catch (const std::system_error&) {
    g_fd = -1;
    close(fd);
    return false;
  }
  send_board();
  send_state(true);
  return true;
}

// Until a window stays, or there's no point trying any more.
void connect_until_one_stays() {
  while (!g_closed && !connect_window()) {
    if (++g_quick_failures >= kFailuresBeforeGivingUp) give_up();
  }
}

// After the reader has finished with the connection, so nothing else reads or closes it.
void forget_connection() {
  const int fd = g_fd.exchange(-1);
  if (fd >= 0) close(fd);
  g_dropped = false;
  if (Clock::now() - g_connected_at < kQuickFailure) {
    if (++g_quick_failures >= kFailuresBeforeGivingUp) give_up();
  } else {
    g_quick_failures = 0;
  }
}

}  // namespace

void open() {
  g_supervised = from_env("PREFLASH_SUPERVISED", "") == "1";
  const std::string dir = app_dir();
  g_socket = from_env("PREFLASH_WINDOW_SOCKET", dir + "/preflash.window");
  g_exe = from_env("PREFLASH_WINDOW_EXE", dir + "/preflash-window");
  g_frame_path = from_env("PREFLASH_FRAME_PATH", dir + "/panel.frame");
  try {
    // Before the window hears of it, so that the file it opens is this app's.
    panel::share_frames(g_frame_path);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "preflash: couldn't share frames with the window: %s\n", e.what());
    socket::leave(1);
  }
  sdl_audio::listen_for_codec();
  connect_until_one_stays();
}

void run() {
  for (;;) {
    sdl_audio::open_if_asked();
    panel::share_pending();
    if (!g_closed) {
      if (g_dropped) {
        forget_connection();
        connect_until_one_stays();
      } else {
        send_state(false);
      }
    }
    std::this_thread::sleep_for(kTurn);
  }
}

void close_for_good() {
  g_closed = true;
  const int fd = g_fd;
  if (fd < 0) return;
  // The newline first ends any line the main thread was part way through, which the window
  // then drops as bad, so the close arrives on a line of its own.
  static constexpr char kClose[] = "\n{\"op\":\"close\"}\n";
  (void)!write(fd, kClose, sizeof kClose - 1);
}

bool supervised() { return g_supervised; }

}  // namespace preflash::window
