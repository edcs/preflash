#include "preflash/trace.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "esp_timer.h"
#include "preflash/serial.h"

namespace preflash::trace {

std::atomic<bool> g_on{false};

namespace {

constexpr uint64_t kSlots = 8192;  // a power of two, so seq maps to a slot by masking
constexpr auto kCopyEvery = std::chrono::milliseconds(20);
constexpr size_t kBytesShown = 40;

// Writers claim a sequence number and then a slot's flag, which nobody holds for longer
// than a copy. A writer that finds the flag taken has lapped the reader, and gives its record
// up instead of waiting.
struct Slot {
  std::atomic_flag busy = ATOMIC_FLAG_INIT;
  uint64_t filled = UINT64_MAX;  // the seq this slot holds, guarded by busy
  Record record;
};

std::array<Slot, kSlots> g_slots;
std::atomic<uint64_t> g_next{0};
std::atomic<uint64_t> g_written{0};
std::atomic<uint64_t> g_collisions{0};

struct Readers {
  std::mutex mutex;
  std::condition_variable someone;
  std::map<int, Subscriber> subscribers;
  int next_subscription = 0;
  uint64_t copied_to = 0;     // the next seq to send out
  uint64_t stalled_at = UINT64_MAX;  // a seq that wasn't ready last time
  bool thread_started = false;
};

Readers& readers() {
  static Readers* r = new Readers;  // never destroyed: its thread outlives static destructors
  return *r;
}

const char* stream_name(Stream stream) {
  switch (stream) {
    case Stream::I2c: return "i2c";
    case Stream::Gpio: return "gpio";
    case Stream::Task: return "task";
    case Stream::Nvs: return "nvs";
    case Stream::Net: return "net";
  }
  return "";
}

void copy_text(char* to, size_t size, const char* from) {
  std::strncpy(to, from, size - 1);
  to[size - 1] = '\0';
}

enum class Read { Got, NotYet, Gone };

Read read_slot(uint64_t seq, Record* out) {
  Slot& slot = g_slots[seq & (kSlots - 1)];
  if (slot.busy.test_and_set(std::memory_order_acquire)) return Read::NotYet;
  Read result = Read::NotYet;
  if (slot.filled == seq) {
    *out = slot.record;
    result = Read::Got;
  } else if (slot.filled != UINT64_MAX && slot.filled > seq) {
    result = Read::Gone;
  }
  slot.busy.clear(std::memory_order_release);
  return result;
}

uint64_t oldest_kept() {
  const uint64_t next = g_next.load(std::memory_order_acquire);
  return next > kSlots ? next - kSlots : 0;
}

// The mutex held. A slot not ready yet gets until the next pass, as a writer may be part way
// through, and is then given up on, as its writer may have lost a collision.
void copy_out(Readers& r) {
  const uint64_t end = g_next.load(std::memory_order_acquire);
  r.copied_to = std::max(r.copied_to, oldest_kept());
  while (r.copied_to < end) {
    Record record;
    const Read result = read_slot(r.copied_to, &record);
    if (result == Read::NotYet && r.stalled_at != r.copied_to) {
      r.stalled_at = r.copied_to;
      return;
    }
    if (result == Read::Got) {
      for (auto& [id, subscriber] : r.subscribers) subscriber(record);
    }
    ++r.copied_to;
  }
}

void copy_forever() {
  serial::mask_signals_on_this_thread();
  Readers& r = readers();
  for (;;) {
    {
      std::unique_lock<std::mutex> lock(r.mutex);
      r.someone.wait(lock, [&] { return !r.subscribers.empty(); });
      copy_out(r);
    }
    std::this_thread::sleep_for(kCopyEvery);
  }
}

template <typename Visit>
void visit_ring(uint64_t from, uint64_t to, Visit visit) {
  for (uint64_t seq = from; seq < to; ++seq) {
    Record record;
    if (read_slot(seq, &record) == Read::Got) visit(record);
  }
}

void add(Stream stream, const char* subject, const char* op, const std::string& data, bool injected,
         int bus, uint8_t address) {
  const uint64_t seq = g_next.fetch_add(1, std::memory_order_acq_rel);
  Slot& slot = g_slots[seq & (kSlots - 1)];
  if (slot.busy.test_and_set(std::memory_order_acquire)) {
    g_collisions.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Record& r = slot.record;
  r.seq = seq;
  r.us = esp_timer_get_time();
  r.stream = stream;
  r.injected = injected;
  r.bus = static_cast<int8_t>(bus);
  r.address = address;
  copy_text(r.subject, sizeof r.subject, subject);
  copy_text(r.op, sizeof r.op, op);
  copy_text(r.data, sizeof r.data, data.c_str());
  slot.filled = seq;
  slot.busy.clear(std::memory_order_release);
  g_written.fetch_add(1, std::memory_order_relaxed);
}

std::string hex(std::span<const uint8_t> bytes) {
  std::string out;
  char text[4];
  for (const uint8_t byte : bytes.first(std::min(bytes.size(), kBytesShown))) {
    std::snprintf(text, sizeof text, " %02x", byte);
    out += text;
  }
  if (bytes.size() > kBytesShown) out += " ...";
  return out.empty() ? out : out.substr(1);
}

// A register chip's first byte picks the register, and what follows it is data.
std::string i2c_data(std::span<const uint8_t> tx, std::span<const uint8_t> rx) {
  const std::span<const uint8_t> head = rx.empty() ? tx.first(std::min<size_t>(tx.size(), 1)) : tx;
  const std::string tail = hex(rx.empty() ? tx.subspan(head.size()) : rx);
  return hex(head) + ":" + (tail.empty() ? "" : " " + tail);
}

}  // namespace

void note(Stream stream, const char* subject, const char* op, const std::string& data,
          bool injected) {
  add(stream, subject, op, data, injected, 0, 0);
}

void i2c(int bus, uint8_t address, std::span<const uint8_t> tx, std::span<const uint8_t> rx,
         bool acked, bool injected) {
  char subject[16];
  if (bus == 0) {
    std::snprintf(subject, sizeof subject, "0x%02x", address);
  } else {
    std::snprintf(subject, sizeof subject, "%d:0x%02x", bus, address);
  }
  const std::string data = injected ? "NACK (injected)" : !acked ? "NACK" : i2c_data(tx, rx);
  add(Stream::I2c, subject, rx.empty() ? "write" : "read", data, injected, bus, address);
}

int subscribe(Subscriber subscriber) {
  Readers& r = readers();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.thread_started) {
    std::thread(copy_forever).detach();
    r.thread_started = true;
  }
  g_on.store(true, std::memory_order_relaxed);
  copy_out(r);
  visit_ring(oldest_kept(), r.copied_to, subscriber);
  const int id = r.next_subscription++;
  r.subscribers[id] = std::move(subscriber);
  r.someone.notify_all();
  return id;
}

void unsubscribe(int subscription) {
  Readers& r = readers();
  std::lock_guard<std::mutex> lock(r.mutex);
  r.subscribers.erase(subscription);
  if (r.subscribers.empty()) g_on.store(false, std::memory_order_relaxed);
}

void start_early() { g_on.store(true, std::memory_order_relaxed); }

size_t save(const std::string& path) {
  Readers& r = readers();
  std::string text;
  size_t saved = 0;
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    visit_ring(oldest_kept(), g_next.load(std::memory_order_acquire), [&](const Record& record) {
      text += to_json(record).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
      text += '\n';
      ++saved;
    });
  }
  // Beside the target and renamed, so a failed save leaves any file already there.
  const std::string temp = path + ".tmp";
  const auto failed = [&path](int error) {
    return std::runtime_error("couldn't save the trace to " + path + ": " + std::strerror(error));
  };
  const int fd = open(temp.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0600);
  if (fd < 0) throw failed(errno);
  const char* at = text.data();
  size_t left = text.size();
  while (left > 0) {
    const ssize_t wrote = write(fd, at, left);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      const int error = errno;
      close(fd);
      unlink(temp.c_str());
      throw failed(error);
    }
    at += wrote;
    left -= static_cast<size_t>(wrote);
  }
  if (close(fd) != 0 || rename(temp.c_str(), path.c_str()) != 0) {
    const int error = errno;
    unlink(temp.c_str());
    throw failed(error);
  }
  return saved;
}

nlohmann::json to_json(const Record& record) {
  nlohmann::json row = {{"stream", stream_name(record.stream)},
                        {"seq", record.seq},
                        {"us", record.us},
                        {"subject", record.subject},
                        {"op", record.op},
                        {"detail", record.data},
                        {"injected", record.injected}};
  if (record.stream == Stream::I2c) {
    row["bus"] = record.bus;
    row["address"] = record.address;
    row["device"] = device_name(board(), record.bus, record.address);
  }
  return row;
}

std::string device_name(const Board& board, int bus, uint8_t address) {
  for (const ChipEntry& chip : board.i2c) {
    if (chip.bus != bus || chip.address != address) continue;
    std::string name = chip.chip;
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return name;
  }
  return "";
}

uint64_t written() { return g_written.load(std::memory_order_relaxed); }

uint64_t collisions() { return g_collisions.load(std::memory_order_relaxed); }

}  // namespace preflash::trace
