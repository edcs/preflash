#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <nlohmann/json.hpp>
#include <span>
#include <string>

#include "preflash/board.h"

// What firmware does to the simulated hardware, as typed records with timestamps. Firmware
// writes to a ring without taking a lock or waiting for anything, and a thread of this
// file's own copies records out to subscribers, whose queues drop the oldest if they fall
// behind. Tracing starts when the first subscriber asks and stops with the last, and until
// then each hook below costs the one relaxed load in on().
namespace preflash::trace {

enum class Stream : uint8_t { I2c, Gpio, Task, Nvs, Net };

struct Record {
  uint64_t seq;   // counts every record of this boot, so a gap shows some were lost
  int64_t us;     // since boot
  Stream stream;
  bool injected;  // a forced failure caused this
  int8_t bus;     // i2c only
  uint8_t address;
  char subject[32];
  char op[16];
  char data[128];
};

extern std::atomic<bool> g_on;

// Check this before building anything to record.
inline bool on() { return g_on.load(std::memory_order_relaxed); }

// Adds a record. Text too long for its field is cut. Never waits.
void note(Stream stream, const char* subject, const char* op, const std::string& data,
          bool injected = false);

// One I2C transfer, with what crossed the bus or NACK.
void i2c(int bus, uint8_t address, std::span<const uint8_t> tx, std::span<const uint8_t> rx,
         bool acked, bool injected);

// Called on this file's thread, with what the ring holds and then everything after it, in
// order. The callback mustn't block. Returns a number for unsubscribe. Throws
// std::system_error if the thread can't start.
using Subscriber = std::function<void(const Record&)>;
int subscribe(Subscriber subscriber);
void unsubscribe(int subscription);

// Tracing with nobody subscribed, for the moments between one boot starting and its first
// client arriving. The next last subscriber to leave stops it.
void start_early();

// Writes what the ring holds as JSON lines, private to the user. Returns how many, and
// throws std::runtime_error saying why it couldn't.
size_t save(const std::string& path);

// A record as the socket and a saved trace show it.
nlohmann::json to_json(const Record& record);

// The chip the board has at that place, in capitals, or empty.
std::string device_name(const Board& board, int bus, uint8_t address);

// Records written since boot, and those a full ring's collision cost.
uint64_t written();
uint64_t collisions();

}  // namespace preflash::trace
