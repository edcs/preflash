#include "preflash/serial.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <system_error>
#include <thread>

#include "preflash/crash.h"
#include "preflash/port.h"

namespace preflash::serial {
namespace {

constexpr size_t kRingBytes = 256 * 1024;
constexpr size_t kPumpChunk = 4096;
// Replayed in pieces, so that a client too slow for a whole ring loses only the oldest.
constexpr size_t kReplayChunk = 16 * 1024;
constexpr int kPumpWaitMs = 1000;
constexpr int kSendWaitMs = 500;
constexpr int kFirstFreeFd = STDERR_FILENO + 1;
constexpr size_t kTerminalQueueBytes = 1024 * 1024;

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::atomic<long> g_unsent{0};

// Plain values and atomics only, because a signal handler reads them.
std::atomic<bool> g_started{false};
std::atomic<bool> g_handed_back{false};
std::atomic<bool> g_pump_done{false};
int g_real_stdout = -1;
int g_real_stderr = -1;

struct Pump {
  std::mutex mutex;
  Ring ring{kRingBytes};
  std::map<int, Subscriber> subscribers;
  int next_subscription = 0;
};

Pump& pump_state() {
  static Pump* p = new Pump;  // never destroyed: the pump outlives static destructors
  return *p;
}

void sleep_ms() {
  const timespec ms = {0, 1000000};
  nanosleep(&ms, nullptr);
}

// What's on its way to the real stdout. A terminal stopped by ^S or a pipe nobody reads
// must not stop the pump, or the pipe behind it fills and firmware blocks, so this has its
// own thread, and past its limit the oldest output goes.
struct Terminal {
  std::mutex mutex;
  std::condition_variable ready;
  std::deque<std::string> chunks;
  size_t bytes = 0;
  size_t dropped = 0;
  bool gone = false;
};

Terminal& terminal() {
  static Terminal* t = new Terminal;  // never destroyed: its thread outlives static destructors
  return *t;
}

// A real stdout that went away must not take the firmware with it, so the ring and
// subscribers carry on without it.
void write_real(int* fd, const char* data, size_t size) {
  while (*fd >= 0 && size > 0) {
    const ssize_t wrote = write(*fd, data, size);
    if (wrote >= 0) {
      data += wrote;
      size -= static_cast<size_t>(wrote);
    } else if (errno == EAGAIN) {
      pollfd ready = {*fd, POLLOUT, 0};
      poll(&ready, 1, -1);
    } else if (errno != EINTR) {
      *fd = -1;
    }
  }
}

void write_to_terminal() {
  mask_signals_on_this_thread();
  Terminal& t = terminal();
  int out = g_real_stdout;
  for (;;) {
    std::string chunk;
    size_t dropped;
    {
      std::unique_lock<std::mutex> lock(t.mutex);
      t.ready.wait(lock, [&] { return !t.chunks.empty() || t.dropped > 0; });
      dropped = t.dropped;
      t.dropped = 0;
      if (!t.chunks.empty()) {
        chunk = std::move(t.chunks.front());
        t.chunks.pop_front();
        t.bytes -= chunk.size();
      }
    }
    if (dropped > 0) {
      const std::string notice = "preflash: " + std::to_string(dropped) +
                                 " bytes of serial dropped because this terminal fell behind\n";
      write_real(&out, notice.data(), notice.size());
    }
    if (!chunk.empty()) {
      write_real(&out, chunk.data(), chunk.size());
      --g_unsent;
    }
    if (out < 0) {
      std::lock_guard<std::mutex> lock(t.mutex);
      t.gone = true;
      g_unsent -= static_cast<long>(t.chunks.size());
      t.chunks.clear();
      return;
    }
  }
}

void queue_for_terminal(const char* data, size_t size) {
  Terminal& t = terminal();
  {
    std::lock_guard<std::mutex> lock(t.mutex);
    if (t.gone) return;
    t.chunks.emplace_back(data, size);
    t.bytes += size;
    ++g_unsent;
    while (t.bytes > kTerminalQueueBytes) {
      t.bytes -= t.chunks.front().size();
      t.dropped += t.chunks.front().size();
      t.chunks.pop_front();
      --g_unsent;
    }
  }
  t.ready.notify_one();
}

void pump(int from) {
  mask_signals_on_this_thread();
  Pump& p = pump_state();
  char buffer[kPumpChunk];
  for (;;) {
    const ssize_t got = read(from, buffer, sizeof buffer);
    if (got == 0) break;
    if (got < 0) {
      if (errno == EINTR) continue;
      break;
    }
    {
      std::lock_guard<std::mutex> lock(p.mutex);
      p.ring.append(buffer, static_cast<size_t>(got));
      for (auto& [id, subscriber] : p.subscribers) subscriber(buffer, static_cast<size_t>(got));
    }
    port::write(buffer, static_cast<size_t>(got));
    queue_for_terminal(buffer, static_cast<size_t>(got));
  }
  close(from);
  g_pump_done = true;
}

// Above the standard three, in case one of them was closed when the pipe was made.
int above_standard_fds(int fd) {
  const int moved = fcntl(fd, F_DUPFD_CLOEXEC, kFirstFreeFd);
  const int saved = errno;
  close(fd);
  errno = saved;
  return moved;
}

}  // namespace

std::string base64(const void* data, size_t size) {
  const auto* in = static_cast<const unsigned char*>(data);
  std::string out;
  out.reserve((size + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 2 < size; i += 3) {
    const unsigned v = (in[i] << 16) | (in[i + 1] << 8) | in[i + 2];
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += kAlphabet[(v >> 6) & 63];
    out += kAlphabet[v & 63];
  }
  if (const size_t left = size - i; left > 0) {
    const unsigned v = (in[i] << 16) | (left == 2 ? in[i + 1] << 8 : 0);
    out += kAlphabet[v >> 18];
    out += kAlphabet[(v >> 12) & 63];
    out += left == 2 ? kAlphabet[(v >> 6) & 63] : '=';
    out += '=';
  }
  return out;
}

Ring::Ring(size_t capacity) : bytes_(capacity) {}

void Ring::append(const char* data, size_t size) {
  if (size >= bytes_.size()) {
    std::memcpy(bytes_.data(), data + size - bytes_.size(), bytes_.size());
    next_ = 0;
    full_ = true;
    return;
  }
  const size_t first = std::min(size, bytes_.size() - next_);
  std::memcpy(bytes_.data() + next_, data, first);
  std::memcpy(bytes_.data(), data + first, size - first);
  if (next_ + size >= bytes_.size()) full_ = true;
  next_ = (next_ + size) % bytes_.size();
}

std::string Ring::contents() const {
  if (!full_) return std::string(bytes_.data(), next_);
  std::string out(bytes_.data() + next_, bytes_.size() - next_);
  out.append(bytes_.data(), next_);
  return out;
}

Outbox::Outbox(size_t serial_limit) : serial_limit_(serial_limit) {}

Outbox::~Outbox() {
  abandon();
  // Nothing is left to call sent() by now.
  forget(unsent_);
}

void Outbox::forget(long items) {
  unsent_ -= items;
  g_unsent -= items;
}

void Outbox::push_message(std::string line) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    message_bytes_ += line.size();
    items_.push_back({std::move(line), Kind::Message});
    forget(-1);
  }
  ready_.notify_one();
}

void Outbox::push_urgent(std::string line) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    message_bytes_ += line.size();
    items_.push_front({std::move(line), Kind::Message});
    forget(-1);
  }
  ready_.notify_one();
}

void Outbox::push_serial(const char* data, size_t size) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    items_.push_back({std::string(data, size), Kind::Serial});
    serial_bytes_ += size;
    forget(-1);
    for (auto it = items_.begin(); serial_bytes_ > serial_limit_ && it != items_.end();) {
      if (it->kind != Kind::Serial) {
        ++it;
        continue;
      }
      serial_bytes_ -= it->bytes.size();
      dropped_ += it->bytes.size();
      it = items_.erase(it);
      forget(1);
    }
  }
  ready_.notify_one();
}

void Outbox::push_trace(std::string line) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    trace_bytes_ += line.size();
    items_.push_back({std::move(line), Kind::Trace});
    forget(-1);
    for (auto it = items_.begin(); trace_bytes_ > serial_limit_ && it != items_.end();) {
      if (it->kind != Kind::Trace) {
        ++it;
        continue;
      }
      trace_bytes_ -= it->bytes.size();
      it = items_.erase(it);
      forget(1);
    }
  }
  ready_.notify_one();
}

bool Outbox::pop(std::string* line) {
  std::unique_lock<std::mutex> lock(mutex_);
  ready_.wait(lock, [this] { return !items_.empty() || dropped_ > 0 || closed_; });
  if (dropped_ > 0) {
    *line = "{\"event\":\"dropped\",\"bytes\":" + std::to_string(dropped_) + "}\n";
    dropped_ = 0;
    forget(-1);
    in_flight_ = 1;
    return true;
  }
  if (items_.empty()) return false;
  Item item = std::move(items_.front());
  items_.pop_front();
  in_flight_ = 1;
  if (item.kind == Kind::Serial) {
    serial_bytes_ -= item.bytes.size();
    lock.unlock();
    *line = "{\"event\":\"serial\",\"data\":\"" + base64(item.bytes.data(), item.bytes.size()) + "\"}\n";
  } else {
    (item.kind == Kind::Trace ? trace_bytes_ : message_bytes_) -= item.bytes.size();
    lock.unlock();
    *line = std::move(item.bytes);
    *line += '\n';
  }
  return true;
}

void Outbox::sent() {
  std::lock_guard<std::mutex> lock(mutex_);
  forget(in_flight_);
  in_flight_ = 0;
}

void Outbox::close() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
  }
  ready_.notify_all();
}

void Outbox::abandon() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    items_.clear();
    dropped_ = 0;
    serial_bytes_ = 0;
    trace_bytes_ = 0;
    message_bytes_ = 0;
    // A line popped but not yet sent stays counted until sent() says it's out.
    forget(unsent_ - in_flight_);
  }
  ready_.notify_all();
}

size_t Outbox::message_bytes() {
  std::lock_guard<std::mutex> lock(mutex_);
  return message_bytes_;
}

long unsent() { return g_unsent; }

bool start() {
  int ends[2];
  if (pipe(ends) != 0) {
    std::fprintf(stderr, "preflash: no pipe for the serial stream: %s\n", std::strerror(errno));
    return false;
  }
  const int read_end = above_standard_fds(ends[0]);
  const int read_errno = errno;
  const int write_end = above_standard_fds(ends[1]);
  // Either can be closed already, which hand_back puts back as it found it. Only that
  // failure counts as closed: anything else would have hand_back close an open fd.
  bool saved = read_end >= 0 && write_end >= 0;
  const char* why = std::strerror(read_end < 0 ? read_errno : errno);
  if (saved) {
    g_real_stdout = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, kFirstFreeFd);
    saved = g_real_stdout >= 0 || errno == EBADF;
    why = std::strerror(errno);
  }
  if (saved) {
    g_real_stderr = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, kFirstFreeFd);
    saved = g_real_stderr >= 0 || errno == EBADF;
    why = std::strerror(errno);
  }
  const auto give_up = [&](const char* reason) {
    for (const int fd : {read_end, write_end, g_real_stdout, g_real_stderr}) {
      if (fd >= 0) close(fd);
    }
    g_real_stdout = -1;
    g_real_stderr = -1;
    std::fprintf(stderr, "preflash: no pipe for the serial stream: %s\n", reason);
    return false;
  };
  if (!saved) return give_up(why);
  try {
    std::thread(write_to_terminal).detach();
    std::thread(pump, read_end).detach();
  } catch (const std::system_error& e) {
    return give_up(e.what());
  }
  std::fflush(stdout);
  std::fflush(stderr);
  dup2(write_end, STDOUT_FILENO);
  dup2(write_end, STDERR_FILENO);
  // Only fds 1 and 2 hold the pipe now, so putting them back ends the pump.
  close(write_end);
  g_started = true;
  // macOS raises SIGPIPE for a pipe at the process, past any thread's mask, so a reader of
  // the real stdout going away would stop the firmware.
  signal(SIGPIPE, SIG_IGN);
  crash::install();
  return true;
}

int subscribe(Subscriber subscriber) {
  Pump& p = pump_state();
  std::lock_guard<std::mutex> lock(p.mutex);
  const std::string replay = p.ring.contents();
  for (size_t at = 0; at < replay.size(); at += kReplayChunk) {
    subscriber(replay.data() + at, std::min(kReplayChunk, replay.size() - at));
  }
  const int id = p.next_subscription++;
  p.subscribers[id] = std::move(subscriber);
  return id;
}

void unsubscribe(int subscription) {
  Pump& p = pump_state();
  std::lock_guard<std::mutex> lock(p.mutex);
  p.subscribers.erase(subscription);
}

void notice(const std::string& text) {
  if (!g_started || g_handed_back) {
    std::fputs(text.c_str(), stderr);
    return;
  }
  Pump& p = pump_state();
  {
    std::lock_guard<std::mutex> lock(p.mutex);
    p.ring.append(text.data(), text.size());
    for (auto& [id, subscriber] : p.subscribers) subscriber(text.data(), text.size());
  }
  queue_for_terminal(text.data(), text.size());
}

std::string ring_contents() {
  Pump& p = pump_state();
  std::lock_guard<std::mutex> lock(p.mutex);
  return p.ring.contents();
}

void mask_signals_on_this_thread() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  sigaddset(&set, SIGTERM);
  sigaddset(&set, SIGHUP);
  pthread_sigmask(SIG_BLOCK, &set, nullptr);
}

void flush_stdio() {
  for (FILE* stream : {stdout, stderr}) {
    if (ftrylockfile(stream) != 0) continue;
    std::fflush(stream);
    funlockfile(stream);
  }
}

void hand_back() {
  if (!g_started) return;
  if (!g_handed_back.exchange(true)) {
    for (const auto [fd, real] : {std::pair{STDOUT_FILENO, g_real_stdout}, std::pair{STDERR_FILENO, g_real_stderr}}) {
      if (real >= 0) {
        dup2(real, fd);
      } else {
        close(fd);
      }
    }
  }
  for (int ms = 0; ms < kPumpWaitMs && !g_pump_done; ++ms) sleep_ms();
}

void wait_sent() {
  for (int ms = 0; ms < kSendWaitMs && g_unsent > 0; ++ms) sleep_ms();
  port::wait_sent();
}

void release() {
  hand_back();
  wait_sent();
  port::remove_links();
}

}  // namespace preflash::serial
