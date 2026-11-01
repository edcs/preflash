#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>

#include "preflash/board.h"
#include "preflash/bus.h"

namespace preflash {

// The common I2C register interface: the first byte of a write selects a
// register, and each byte after it, written or read, moves to the next.
class RegisterChip : public I2cDevice {
 public:
  explicit RegisterChip(std::string name);

  bool write(std::span<const uint8_t> data) override;
  bool read(std::span<uint8_t> out) override;

 protected:
  virtual uint8_t load(uint8_t reg) { return regs_[reg]; }
  virtual void store(uint8_t reg, uint8_t value) { regs_[reg] = value; }
  virtual uint8_t next(uint8_t reg) { return static_cast<uint8_t>(reg + 1); }

  // Stops the run, naming this chip and the register.
  [[noreturn]] void refuse(uint8_t reg, const char* why) const;

  std::array<uint8_t, 256> regs_{};

 private:
  std::string name_;
  uint8_t address_ = 0;
};

// NXP PCF85063A, datasheet rev. 7. Starts from the Mac's clock in UTC. Its
// registers keep running through esp_restart, as a chip on its own cell does.
class Pcf85063 : public RegisterChip {
 public:
  Pcf85063();
  // Counts by a clock of the caller's instead of the Mac's, which tests move by hand.
  explicit Pcf85063(std::function<std::time_t()> clock);
  ~Pcf85063() override;

  bool write(std::span<const uint8_t> data) override;
  bool read(std::span<uint8_t> out) override;

  // Sets or clears the oscillator stop flag, as a flat backup cell would.
  void set_lost_power(bool lost) { oscillator_stopped_ = lost; }

  // Leaves the registers in the environment for the next process to start from.
  void save_for_restart();

 protected:
  uint8_t load(uint8_t reg) override;
  void store(uint8_t reg, uint8_t value) override;
  uint8_t next(uint8_t reg) override;

 private:
  void restore(const std::string& carried);
  void set_fields(const std::tm& t);
  void count_time();

  std::function<std::time_t()> clock_;
  std::mutex mutex_;  // a restart reads the registers from another task
  std::time_t counted_to_ = 0;  // the moment the time registers show
  std::atomic<bool> oscillator_stopped_{false};
};

// QST QMI8658, datasheet QMI8658C rev. 0.9. Stands still and upright, so the
// accelerometer reads +1 g on Y. The gyroscope and everything built on it stop
// the run if turned on.
class Qmi8658 : public RegisterChip {
 public:
  Qmi8658();

 protected:
  uint8_t load(uint8_t reg) override;
  void store(uint8_t reg, uint8_t value) override;
  uint8_t next(uint8_t reg) override;

 private:
  void reset();
  void take_sample();
};

// X-Powers AXP2101, datasheet V1.0. USB is in and no battery is fitted.
class Axp2101 : public RegisterChip {
 public:
  Axp2101();

 protected:
  uint8_t load(uint8_t reg) override;
  void store(uint8_t reg, uint8_t value) override;
};

// Called by esp_restart just before it re-runs the process, so that chips which
// keep their state through a restart can hand it on.
void carry_chips_across_restart();

using ChipFactory = std::function<std::unique_ptr<I2cDevice>()>;

void register_chip(const std::string& name, ChipFactory factory);

// Puts a model of each of the board's chips on the bus. Throws, naming the chip,
// if one has no model.
void attach_chips(const Board& board);

}  // namespace preflash
