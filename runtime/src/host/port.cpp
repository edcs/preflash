#include "preflash/port.h"

#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <sys/stat.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <system_error>
#include <thread>

#include "preflash/control.h"
#include "preflash/serial.h"

namespace preflash::port {
namespace {

constexpr const char* kLinkEnv = "PREFLASH_PORT";
constexpr const char* kExtraLinkEnv = "PREFLASH_PORT_LINK";
constexpr const char* kFdsEnv = "PREFLASH_PORT_FDS";

// Small, because with nobody attached this sits full, and an app that opens the port later
// reads it first. The terminal itself holds about 1 KiB.
constexpr size_t kQueueBytes = 64 * 1024;
// As much as the console queue can take at once.
constexpr size_t kLongestLine = 1024 * 1024;
constexpr size_t kReadChunk = 4096;
constexpr int kFirstFreeFd = STDERR_FILENO + 1;
constexpr int kLinks = 2;
constexpr int kBusyMs = 10;
// How long leaving waits for a reader, in all and with nothing moving. A full terminal
// usually means nobody is there, so this has to be short.
constexpr int kSendWaitMs = 500;
constexpr int kStallMs = 50;

// Plain values and atomics only, because a signal handler reads them.
int g_master = -1;
int g_slave = -1;
char g_device[PATH_MAX] = "";
char g_links[kLinks][PATH_MAX] = {};
std::atomic<bool> g_writing{false};
std::atomic<bool> g_holding{false};
std::atomic<size_t> g_unsent{0};
std::atomic<uint64_t> g_moved{0};

std::string g_path;

struct Queue {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::string> chunks;
  size_t bytes = 0;
};

Queue& queue() {
  static Queue* q = new Queue;  // never destroyed: its thread outlives static destructors
  return *q;
}

// Milliseconds on the monotonic clock, which a signal handler can read.
int64_t now_ms() {
  timespec now {};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return int64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

void sleep_ms(int ms) {
  const timespec wait = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&wait, nullptr);
}

void write_chunk(const std::string& chunk) {
  const char* at = chunk.data();
  size_t left = chunk.size();
  while (left > 0) {
    const ssize_t wrote = ::write(g_master, at, left);
    if (wrote > 0) {
      at += wrote;
      left -= static_cast<size_t>(wrote);
      g_unsent -= static_cast<size_t>(wrote);
      ++g_moved;
    } else if (wrote == 0 || errno == EAGAIN) {
      pollfd ready = {g_master, POLLOUT, 0};
      poll(&ready, 1, -1);
    } else if (errno != EINTR) {
      g_unsent -= left;
      return;
    }
  }
}

void write_queue() {
  serial::mask_signals_on_this_thread();
  Queue& q = queue();
  for (;;) {
    std::string chunk;
    {
      std::unique_lock<std::mutex> lock(q.mutex);
      q.ready.wait(lock, [&] { return !q.chunks.empty(); });
      chunk = std::move(q.chunks.front());
      q.chunks.pop_front();
      q.bytes -= chunk.size();
    }
    write_chunk(chunk);
  }
}

// The app's own words stay off the port, which carries only what firmware says.
void take_line(const std::string& line) {
  if (!submit_line(line)) serial::notice("preflash: the console queue is full, so a line was dropped\n");
}

void take_bytes(const char* data, size_t size) {
  if (!submit_bytes(data, size)) {
    serial::notice("preflash: the console queue is full, so serial input was dropped\n");
  }
}

void read_input() {
  serial::mask_signals_on_this_thread();
  LineSplitter lines(kLongestLine);
  char buffer[kReadChunk];
  for (;;) {
    pollfd ready = {g_master, POLLIN, 0};
    const int woke = poll(&ready, 1, -1);
    if (g_holding) return;
    if (woke < 0) continue;
    const ssize_t got = read(g_master, buffer, sizeof buffer);
    if (got > 0 && firmware_reads_serial()) {
      const std::string partial = lines.take_partial();
      if (!partial.empty()) take_bytes(partial.data(), partial.size());
      take_bytes(buffer, static_cast<size_t>(got));
    } else if (got > 0) {
      if (lines.feed(buffer, static_cast<size_t>(got), take_line) > 0) {
        serial::notice("preflash: a line on the serial port was too long, so it was dropped\n");
      }
    } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
      sleep_ms(kBusyMs);
    }
  }
}

// Above the standard three, in case one of them was closed when this ran, and closed on
// exec, so that only a restart passes it on.
int above_standard_fds(int fd) {
  const int moved = fcntl(fd, F_DUPFD_CLOEXEC, kFirstFreeFd);
  const int saved = errno;
  close(fd);
  errno = saved;
  return moved;
}

bool take_over() {
  const char* fds = std::getenv(kFdsEnv);
  int master;
  int slave;
  if (!fds || std::sscanf(fds, "%d:%d", &master, &slave) != 2) return false;
  if (master < kFirstFreeFd || slave < kFirstFreeFd) return false;
  // Whatever set the variable, these have to be the two ends of one terminal.
  const char* name = ptsname(master);
  const char* held = ttyname(slave);
  if (!name || !held || std::strcmp(name, held) != 0) return false;
  fcntl(master, F_SETFD, FD_CLOEXEC);
  fcntl(slave, F_SETFD, FD_CLOEXEC);
  g_master = master;
  g_slave = slave;
  return true;
}

bool make_raw(int fd) {
  termios settings{};
  if (tcgetattr(fd, &settings) != 0) return false;
  cfmakeraw(&settings);
  settings.c_cflag |= CLOCAL | CREAD;
  settings.c_cc[VMIN] = 1;
  settings.c_cc[VTIME] = 0;
  cfsetspeed(&settings, B115200);
  return tcsetattr(fd, TCSANOW, &settings) == 0;
}

void complain(const char* what) {
  std::fprintf(stderr, "preflash: no serial port: %s: %s\n", what, std::strerror(errno));
}

bool open_new() {
  const int master = posix_openpt(O_RDWR | O_NOCTTY);
  if (master < 0) {
    complain("posix_openpt");
    return false;
  }
  const char* name = nullptr;
  if (grantpt(master) != 0 || unlockpt(master) != 0 || !(name = ptsname(master))) {
    complain("setting up the terminal");
    close(master);
    return false;
  }
  const int slave = open(name, O_RDWR | O_NOCTTY);
  if (slave < 0) {
    complain(name);
    close(master);
    return false;
  }
  g_master = above_standard_fds(master);
  g_slave = above_standard_fds(slave);
  // A terminal is writable by its group otherwise, and input here reaches the firmware.
  const bool ready = g_master >= 0 && g_slave >= 0 && fchmod(g_slave, 0600) == 0 &&
                     make_raw(g_slave) &&
                     fcntl(g_master, F_SETFL, fcntl(g_master, F_GETFL) | O_NONBLOCK) == 0;
  if (ready) return true;
  complain("setting up the terminal");
  for (int* fd : {&g_master, &g_slave}) {
    if (*fd >= 0) close(*fd);
    *fd = -1;
  }
  return false;
}

bool points_here(const char* link) {
  char target[PATH_MAX];
  const ssize_t got = readlink(link, target, sizeof target);
  return got >= 0 && static_cast<size_t>(got) == std::strlen(g_device) &&
         std::memcmp(target, g_device, static_cast<size_t>(got)) == 0;
}

// Replaces a link in one step, and nothing that isn't a link: the path may be the project's
// choice. Restarts usually find it done already.
bool make_link(const char* link, int slot) {
  if (std::strlen(link) >= sizeof g_links[slot]) {
    std::fprintf(stderr, "preflash: couldn't link the serial port: the path is too long: %s\n", link);
    return false;
  }
  if (!points_here(link)) {
    struct stat info {};
    if (lstat(link, &info) == 0 && !S_ISLNK(info.st_mode)) {
      std::fprintf(stderr, "preflash: %s is in the way of the serial port, and isn't a link\n", link);
      return false;
    }
    const std::string next = std::string(link) + ".next";
    // Only a run that stopped right here leaves a link at that name. A file there is someone's.
    if (lstat(next.c_str(), &info) == 0 && S_ISLNK(info.st_mode)) unlink(next.c_str());
    if (symlink(g_device, next.c_str()) != 0) {
      std::fprintf(stderr, "preflash: couldn't link the serial port at %s: %s\n", link,
                   std::strerror(errno));
      return false;
    }
    if (rename(next.c_str(), link) != 0) {
      std::fprintf(stderr, "preflash: couldn't link the serial port at %s: %s\n", link,
                   std::strerror(errno));
      unlink(next.c_str());
      return false;
    }
  }
  std::strncpy(g_links[slot], link, sizeof g_links[slot] - 1);
  return true;
}

}  // namespace

LineSplitter::LineSplitter(size_t longest) : longest_(longest) {}

std::string LineSplitter::take_partial() {
  std::string partial;
  partial.swap(line_);
  return partial;
}

size_t LineSplitter::feed(const char* data, size_t size, const Emit& emit) {
  size_t dropped = 0;
  for (size_t i = 0; i < size; ++i) {
    const char c = data[i];
    const bool second_half = after_return_ && c == '\n';
    after_return_ = c == '\r';
    if (second_half) continue;
    if (c != '\n' && c != '\r') {
      if (line_.size() < longest_) {
        line_ += c;
      } else {
        too_long_ = true;
      }
      continue;
    }
    if (too_long_) {
      ++dropped;
    } else {
      emit(line_);
    }
    line_.clear();
    too_long_ = false;
  }
  return dropped;
}

void start() {
  const char* link = std::getenv(kLinkEnv);
  if (!link || !*link) return;
  if (!take_over() && !open_new()) return;
  std::strncpy(g_device, ptsname(g_master), sizeof g_device - 1);
  setenv(kFdsEnv, (std::to_string(g_master) + ":" + std::to_string(g_slave)).c_str(), 1);
  const char* extra = std::getenv(kExtraLinkEnv);
  const bool linked = make_link(link, 0);
  const bool extra_linked = extra && *extra && make_link(extra, 1);
  g_path = linked ? link : extra_linked ? extra : g_device;
  try {
    std::thread(write_queue).detach();
    std::thread(read_input).detach();
    g_writing = true;
  } catch (const std::system_error& e) {
    // Half a port would look fine and pass nothing, so nobody gets offered one.
    std::fprintf(stderr, "preflash: no serial port: %s\n", e.what());
    remove_links();
    g_path.clear();
  }
}

void write(const char* data, size_t size) {
  if (!g_writing) return;
  Queue& q = queue();
  {
    std::lock_guard<std::mutex> lock(q.mutex);
    q.chunks.emplace_back(data, size);
    q.bytes += size;
    g_unsent += size;
    while (q.bytes > kQueueBytes) {
      q.bytes -= q.chunks.front().size();
      g_unsent -= q.chunks.front().size();
      q.chunks.pop_front();
    }
  }
  q.ready.notify_one();
}

// Both limits are measured on the clock: a 1 ms sleep can last far longer on a busy machine.
void wait_sent() {
  const int64_t start = now_ms();
  int64_t last_move = start;
  uint64_t moved = g_moved;
  while (g_unsent > 0) {
    const int64_t now = now_ms();
    if (now - start >= kSendWaitMs || now - last_move >= kStallMs) return;
    sleep_ms(1);
    if (g_moved != moved) {
      moved = g_moved;
      last_move = now_ms();
    }
  }
}

void hold_input() { g_holding = true; }

void remove_links() {
  for (const char* link : g_links) {
    if (link[0] && points_here(link)) unlink(link);
  }
}

void hang_up() {
  g_holding = true;
  for (const int fd : {g_master, g_slave}) {
    if (fd >= 0) close(fd);
  }
}

std::string path() { return g_path; }

std::string device() { return g_device; }

int master_fd() { return g_master; }

int slave_fd() { return g_slave; }

}  // namespace preflash::port
