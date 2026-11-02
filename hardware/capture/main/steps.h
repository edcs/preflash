// The driver calls a capture makes, as tables. A step is one I2C transaction, or a wait. Nothing
// here knows about a board: board_*.cpp puts each chip's list on its address.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

constexpr size_t kLongestWrite = 16;
constexpr size_t kLongestRead = 32;

struct Step {
  std::array<uint8_t, kLongestWrite> write = {};
  uint8_t write_length = 0;
  uint8_t read_length = 0;
  uint16_t wait_ms = 0;  // after the transaction, or alone
  bool keep = false;     // remember the first byte read
  bool put_back = false;  // append the byte kept to the write
};

constexpr Step get(uint8_t reg, uint8_t length = 1) { return {.write = {reg}, .write_length = 1, .read_length = length}; }

constexpr Step get16(uint16_t reg, uint8_t length = 1) {
  return {.write = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg)}, .write_length = 2, .read_length = length};
}

constexpr Step set(uint8_t reg, uint8_t value, uint16_t wait_ms = 0) {
  return {.write = {reg, value}, .write_length = 2, .wait_ms = wait_ms};
}

constexpr Step set16(uint16_t reg, uint8_t value) {
  return {.write = {static_cast<uint8_t>(reg >> 8), static_cast<uint8_t>(reg), value}, .write_length = 3};
}

// A register the chip keeps through a power cut, or one that matters to the board, is put back.
constexpr Step keep(uint8_t reg) { return {.write = {reg}, .write_length = 1, .read_length = 1, .keep = true}; }
constexpr Step put_back(uint8_t reg) { return {.write = {reg}, .write_length = 1, .put_back = true}; }

constexpr Step wait(uint16_t ms) { return {.wait_ms = ms}; }

// One chip on the board, and what to do to it.
struct Chip {
  const char* name;
  int bus;
  uint8_t address;
  uint32_t bus_hz;
  std::span<const Step> steps;
};

// A bus and its pins.
struct Bus {
  int port;
  int sda;
  int scl;
};

struct Board {
  const char* name;  // as the board file in boards/ has it
  std::span<const Bus> buses;
  std::span<const Chip> chips;
};

// Each chip's list, in chips.cpp.
extern const std::span<const Step> kTca9554;
extern const std::span<const Step> kEs8311;
extern const std::span<const Step> kEs7210;
extern const std::span<const Step> kGt911;
extern const std::span<const Step> kAxp2101;
extern const std::span<const Step> kPcf85063;
extern const std::span<const Step> kQmi8658;
extern const std::span<const Step> kAxs15231bTouch;

// The board this build is for, in board_*.cpp.
extern const Board kBoard;
