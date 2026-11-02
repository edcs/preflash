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
#include <map>
#include <system_error>
#include <thread>

namespace preflash::serial {
namespace {

constexpr size_t kRingBytes = 256 * 1024;
constexpr size_t kPumpChunk = 4096;
// Replayed in pieces, so that a client too slow for a whole ring loses only the oldest.
constexpr size_t kReplayChunk = 16 * 1024;
constexpr int kPumpWaitMs = 1000;
constexpr int kSendWaitMs = 500;
constexpr int kFirstFreeFd = STDERR_FILENO + 1;

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

// A real stdout that went away must not take the firmware with it, so the pump carries
// on for the ring and subscribers.
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

void pump(int from) {
  mask_signals_on_this_thread();
  Pump& p = pump_state();
  int out = g_real_stdout;
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
    write_real(&out, buffer, static_cast<size_t>(got));
  }
  close(from);
  g_pump_done = true;
}

// A process dying with output still in the pipe would lose the message that says why.
void on_fatal_signal(int signal_number) {
  release();
  signal(signal_number, SIG_DFL);
  raise(signal_number);
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

Outbox::~Outbox() { abandon(); }

void Outbox::forget(long items) {
  unsent_ -= items;
  g_unsent -= items;
}

void Outbox::push_message(std::string line) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    message_bytes_ += line.size();
    items_.push_back({std::move(line), false});
    forget(-1);
  }
  ready_.notify_one();
}

void Outbox::push_serial(const char* data, size_t size) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return;
    items_.push_back({std::string(data, size), true});
    serial_bytes_ += size;
    forget(-1);
    for (auto it = items_.begin(); serial_bytes_ > serial_limit_ && it != items_.end();) {
      if (!it->serial) {
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

bool Outbox::pop(std::string* line) {
  std::unique_lock<std::mutex> lock(mutex_);
  ready_.wait(lock, [this] { return !items_.empty() || dropped_ > 0 || closed_; });
  if (dropped_ > 0) {
    *line = "{\"event\":\"dropped\",\"bytes\":" + std::to_string(dropped_) + "}\n";
    dropped_ = 0;
    forget(-1);
    return true;
  }
  if (items_.empty()) return false;
  Item item = std::move(items_.front());
  items_.pop_front();
  if (item.serial) {
    serial_bytes_ -= item.bytes.size();
    lock.unlock();
    *line = "{\"event\":\"serial\",\"data\":\"" + base64(item.bytes.data(), item.bytes.size()) + "\"}\n";
  } else {
    message_bytes_ -= item.bytes.size();
    lock.unlock();
    *line = std::move(item.bytes);
    *line += '\n';
  }
  return true;
}

void Outbox::sent() {
  std::lock_guard<std::mutex> lock(mutex_);
  forget(1);
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
    message_bytes_ = 0;
    forget(unsent_);
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
  const int write_end = above_standard_fds(ends[1]);
  if (read_end < 0 || write_end < 0) {
    std::fprintf(stderr, "preflash: no pipe for the serial stream: %s\n", std::strerror(errno));
    return false;
  }
  // Either can be closed already, which hand_back puts back as it found it.
  g_real_stdout = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, kFirstFreeFd);
  g_real_stderr = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, kFirstFreeFd);
  try {
    std::thread(pump, read_end).detach();
  } catch (const std::system_error& e) {
    std::fprintf(stderr, "preflash: no thread for the serial stream: %s\n", e.what());
    return false;
  }
  std::fflush(stdout);
  std::fflush(stderr);
  dup2(write_end, STDOUT_FILENO);
  dup2(write_end, STDERR_FILENO);
  // Only fds 1 and 2 hold the pipe now, so putting them back ends the pump.
  close(write_end);
  g_started = true;
  for (const int fatal : {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE}) signal(fatal, on_fatal_signal);
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

std::string ring_contents() {
  Pump& p = pump_state();
  std::lock_guard<std::mutex> lock(p.mutex);
  return p.ring.contents();
}

void mask_signals_on_this_thread() {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGINT);
  // A reader that went away shows up as an error from write.
  sigaddset(&set, SIGPIPE);
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
}

void release() {
  hand_back();
  wait_sent();
}

}  // namespace preflash::serial
