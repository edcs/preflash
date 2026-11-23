#include "preflash/power.h"

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#include "preflash/buttons.h"
#include "preflash/bus.h"
#include "preflash/chips.h"
#include "preflash/fail.h"
#include "preflash/hardware.h"
#include "preflash/port.h"
#include "preflash/serial.h"
#include "preflash/socket.h"
#include "preflash/trace.h"

namespace preflash::power {
namespace {

constexpr const char* kPowerEnv = "PREFLASH_POWER";
constexpr const char* kHeldEnv = "PREFLASH_HELD";
constexpr const char* kResetEnv = "PREFLASH_RESET";
// The I/O expander's registers, which a reset that leaves the board powered keeps.
constexpr const char* kExpanderEnv = "PREFLASH_EXPANDER";
// What runs before this one set for a software reset.
constexpr const char* kOldRestartedEnv = "PREFLASH_RESTARTED";

// The chip samples GPIO0 as it comes out of reset, and low there means download mode.
constexpr int kStrappingPin = 0;

std::atomic<bool> g_entering{false};

bool is_named(const std::string& entry, const char* name) {
  const size_t length = std::strlen(name);
  return entry.size() > length && entry.compare(0, length, name) == 0 && entry[length] == '=';
}

std::mutex g_stand_in_mutex;
std::function<void(Mode, Reset)> g_stand_in;

const Button* power_key(const Board& board) {
  const auto key = std::find_if(board.buttons.begin(), board.buttons.end(),
                                [](const Button& b) { return b.kind == Button::Kind::Power; });
  return key == board.buttons.end() ? nullptr : &*key;
}

bool has_button(const Board& board, const std::string& name, Button::Kind kind) {
  return std::any_of(board.buttons.begin(), board.buttons.end(),
                     [&](const Button& b) { return b.name == name && b.kind == kind; });
}

// What the expander's registers should be in the next run, if it keeps any: the chip's own
// while the board runs, what download mode was handed, and nothing once the power goes,
// because a chip with no power starts afresh.
std::optional<std::string> expander_for(Mode next) {
  if (next == Mode::Off) return std::nullopt;
  switch (mode()) {
    case Mode::On:
      return expander_carry();
    case Mode::Download:
      if (const char* carried = std::getenv(kExpanderEnv)) return std::string(carried);
      return std::nullopt;
    case Mode::Off:
      break;
  }
  return std::nullopt;
}

// A power key's circuit, every kPowerPollMs, on a thread of its own that holds no lock. On,
// the board goes off when decide says so. Off, USB coming back powers it, as at a launch.
// Presses reach watch_power through on_change instead, so a tap shorter than the poll counts.
void watch_circuit(const Board& board, const std::string& key) {
  const Mode now = mode();
  for (;;) {
    const bool usb = hardware::usb_in();
    if (now == Mode::On && decide(usb, buttons::down(key), holding(board)) == Mode::Off) {
      switch_to(Mode::Off, Reset::PowerOn);
      return;
    }
    if (now == Mode::Off && usb) {
      switch_to(Mode::On, Reset::PowerOn);
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(kPowerPollMs));
  }
}

}  // namespace

Mode mode() {
  const char* value = std::getenv(kPowerEnv);
  if (value && std::strcmp(value, "off") == 0) return Mode::Off;
  if (value && std::strcmp(value, "download") == 0) return Mode::Download;
  return Mode::On;
}

const char* name(Mode mode) {
  switch (mode) {
    case Mode::Off:
      return "off";
    case Mode::Download:
      return "download";
    case Mode::On:
      break;
  }
  return "on";
}

std::vector<std::string> env_for(Mode next, Reset reason) {
  static constexpr const char* kReplaced[] = {kPowerEnv, kHeldEnv, kResetEnv, kOldRestartedEnv,
                                              kExpanderEnv};
  std::vector<std::string> env;
  for (char** entry = *_NSGetEnviron(); *entry; ++entry) {
    const std::string text(*entry);
    const bool replaced = std::any_of(std::begin(kReplaced), std::end(kReplaced),
                                      [&text](const char* name) { return is_named(text, name); });
    if (!replaced) env.push_back(text);
  }
  env.push_back(std::string(kPowerEnv) + "=" + name(next));
  if (const std::string held = buttons::held_env(); !held.empty()) {
    env.push_back(std::string(kHeldEnv) + "=" + held);
  }
  env.push_back(std::string(kResetEnv) + "=" + (reason == Reset::Software ? "sw" : "poweron"));
  if (const std::optional<std::string> expander = expander_for(next)) {
    env.push_back(std::string(kExpanderEnv) + "=" + *expander);
  }
  return env;
}

void enter(Mode next, Reset reason) {
  // What a failure names: firmware called esp_restart, or a button or the power reset the chip.
  const char* caller = reason == Reset::Software ? "esp_restart" : "reset";
  // One way out: a reset that comes while another is on its way is part of the same one.
  if (g_entering.exchange(true)) {
    for (;;) pause();
  }
  if (socket::quitting()) socket::quit();
  port::hold_input();
  // Not fflush(nullptr): that also waits on stdin, which the console task keeps
  // locked for as long as it sits in a read.
  std::fflush(stdout);
  std::fflush(stderr);
  char path[4096];
  uint32_t size = sizeof path;
  if (_NSGetExecutablePath(path, &size) != 0) {
    preflash_unsupported(caller, "the executable's path is too long to re-run");
  }
  carry_chips_across_restart();
  if (trace::on()) {
    setenv("PREFLASH_TRACE", "1", 1);
  } else {
    unsetenv("PREFLASH_TRACE");
  }
  // After everything above has had its say in the environment.
  const std::vector<std::string> env = env_for(next, reason);
  std::vector<char*> envp;
  for (const std::string& entry : env) envp.push_back(const_cast<char*>(entry.c_str()));
  envp.push_back(nullptr);
  // The stream's pipe ends with this process, and the next run needs the real fds.
  serial::hand_back();
  socket::announce_restart();
  // A client that let go of RST gets its reply before the exec, which waits for it below.
  socket::wait_for_replies();
  serial::wait_sent();
  // Those waits take a while, and a quit that arrived meanwhile has been told yes.
  if (socket::quitting()) socket::quit();
  exec_clean(path, *_NSGetArgv(), envp.data());
  preflash_unsupported(caller, "re-running the executable failed");
}

std::optional<Mode> after_release(const Board& board, const std::string& released) {
  if (mode() == Mode::Off) return std::nullopt;
  const auto is_reset = [&released](const Button& b) {
    return b.name == released && b.kind == Button::Kind::Reset;
  };
  if (std::none_of(board.buttons.begin(), board.buttons.end(), is_reset)) return std::nullopt;
  // Held when it pulls the pin low, as BOOT does.
  const bool strapped = std::any_of(board.buttons.begin(), board.buttons.end(), [](const Button& b) {
    return b.kind == Button::Kind::Gpio && b.pin == kStrappingPin && b.active_low &&
           buttons::down(b.name);
  });
  return strapped ? Mode::Download : Mode::On;
}

void watch_reset(const Board& board) {
  buttons::on_change([board](const std::string& name, bool down) {
    if (down) return;
    const std::optional<Mode> next = after_release(board, name);
    if (!next) return;
    // On a thread of its own, so that whoever let go of RST can still answer for it: a
    // socket client hears its reply ahead of the restart, more often than not.
    switch_soon(*next, Reset::PowerOn);
  });
}

Mode decide(bool usb, bool key, bool hold) { return usb || key || hold ? Mode::On : Mode::Off; }

bool holding(const Board& board) {
  const Button* key = power_key(board);
  if (!key) return false;
  if (key->hold_expander_bit) {
    const ExpanderPins pins = expander_pins();
    const uint32_t bit = 1u << *key->hold_expander_bit;
    return (pins.outputs & bit) && (pins.levels & bit);
  }
  return key->hold_pin && gpio_is_output(*key->hold_pin) && gpio_read(*key->hold_pin);
}

bool starts_off(const Board& board) {
  const Button* key = power_key(board);
  if (!key || std::getenv(kPowerEnv)) return false;
  return decide(hardware::usb_in(), buttons::down(key->name), false) == Mode::Off;
}

void switch_to(Mode next, Reset reason) {
  std::function<void(Mode, Reset)> stand_in;
  {
    std::lock_guard<std::mutex> lock(g_stand_in_mutex);
    stand_in = g_stand_in;
  }
  if (!stand_in) enter(next, reason);
  stand_in(next, reason);
}

void switch_soon(Mode next, Reset reason) {
  try {
    std::thread([next, reason] { switch_to(next, reason); }).detach();
  } catch (const std::system_error&) {
    switch_to(next, reason);
  }
}

void set_enter_for_test(std::function<void(Mode, Reset)> stand_in) {
  std::lock_guard<std::mutex> lock(g_stand_in_mutex);
  g_stand_in = std::move(stand_in);
}

void watch_power(const Board& board) {
  const Mode now = mode();
  if (now == Mode::Download) return;
  buttons::on_change([board, now](const std::string& name, bool down) {
    const bool axp2101 = has_button(board, name, Button::Kind::Axp2101);
    if (now == Mode::Off) {
      // The AXP2101 wants PWRON held for ONLEVEL first (datasheet section 6.5.4.2), and a
      // power key's circuit powers the board the moment the key goes down. Either way the
      // press boots it, with the key still down.
      if (down && (axp2101 || has_button(board, name, Button::Kind::Power))) {
        switch_soon(Mode::On, Reset::PowerOn);
      }
      return;
    }
    if (axp2101) axp2101_pwron(down);
  });
  const Button* key = power_key(board);
  if (!key) return;
  try {
    std::thread([board, name = key->name] { watch_circuit(board, name); }).detach();
  } catch (const std::system_error&) {
    preflash_unsupported("power", "couldn't start a thread to watch the power key");
  }
}

}  // namespace preflash::power
