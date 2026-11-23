#include "preflash/power.h"

#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <system_error>
#include <thread>

#include "preflash/buttons.h"
#include "preflash/chips.h"
#include "preflash/fail.h"
#include "preflash/port.h"
#include "preflash/serial.h"
#include "preflash/socket.h"
#include "preflash/trace.h"

namespace preflash::power {
namespace {

constexpr const char* kPowerEnv = "PREFLASH_POWER";
constexpr const char* kHeldEnv = "PREFLASH_HELD";
constexpr const char* kResetEnv = "PREFLASH_RESET";
// What runs before this one set for a software reset.
constexpr const char* kOldRestartedEnv = "PREFLASH_RESTARTED";

// The chip samples GPIO0 as it comes out of reset, and low there means download mode.
constexpr int kStrappingPin = 0;

std::atomic<bool> g_entering{false};

bool is_named(const std::string& entry, const char* name) {
  const size_t length = std::strlen(name);
  return entry.size() > length && entry.compare(0, length, name) == 0 && entry[length] == '=';
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
  static constexpr const char* kReplaced[] = {kPowerEnv, kHeldEnv, kResetEnv, kOldRestartedEnv};
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
    try {
      std::thread([next] { enter(*next, Reset::PowerOn); }).detach();
    } catch (const std::system_error&) {
      enter(*next, Reset::PowerOn);
    }
  });
}

}  // namespace preflash::power
