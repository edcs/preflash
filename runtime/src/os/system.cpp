#include <crt_externs.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "preflash/chips.h"
#include "preflash/crash.h"
#include "preflash/fail.h"
#include "preflash/port.h"
#include "preflash/serial.h"
#include "preflash/socket.h"
#include "preflash/trace.h"

namespace {

// Set across a restart so the next run can report a software reset.
constexpr const char* kRestartedEnv = "PREFLASH_RESTARTED";

constexpr size_t kReportedFreeBytes = 256 * 1024;

// Replaces this process with a fresh run of the executable. Plain execv would
// pass on every open descriptor, the calling thread's blocked signals and any
// ignored ones, so this closes every descriptor but the standard three and the
// virtual serial port, and resets the signals in the same step, whichever thread
// opened what. Returns only on failure.
int exec_clean(const char* path, char* const argv[]) {
  posix_spawnattr_t attr;
  posix_spawn_file_actions_t actions;
  if (posix_spawnattr_init(&attr) != 0) return -1;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    posix_spawnattr_destroy(&attr);
    return -1;
  }
  sigset_t none;
  sigset_t all;
  sigemptyset(&none);
  sigfillset(&all);
  // main keeps these ignored only if they arrived that way, as under nohup, and the next
  // run must inherit that, unlike any other ignored signal.
  for (const int stop : {SIGTERM, SIGHUP}) {
    struct sigaction current {};
    if (sigaction(stop, nullptr, &current) == 0 && current.sa_handler == SIG_IGN) {
      sigdelset(&all, stop);
    }
  }
  int err = posix_spawnattr_setsigmask(&attr, &none);
  if (!err) err = posix_spawnattr_setsigdefault(&attr, &all);
  if (!err) {
    err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_CLOEXEC_DEFAULT |
                                              POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  }
  const int kept[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, preflash::port::master_fd(),
                      preflash::port::slave_fd()};
  for (const int fd : kept) {
    // Inheriting a closed descriptor would fail the whole spawn, and so would one still
    // marked to close on exec.
    if (!err && fd >= 0 && fcntl(fd, F_SETFD, 0) != -1) {
      err = posix_spawn_file_actions_addinherit_np(&actions, fd);
    }
  }
  if (!err) err = posix_spawn(nullptr, path, &actions, &attr, argv, *_NSGetEnviron());
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  return err ? -1 : 0;
}

}  // namespace

extern "C" {

uint32_t esp_log_timestamp(void) { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void esp_log_write(esp_log_level_t level, const char* tag, const char* format, ...) {
  // Devices default to info, so chattier levels stay quiet here too.
  if (level == ESP_LOG_NONE || level > ESP_LOG_INFO) return;
  static const char kLetters[] = "NEWIDV";
  // The lock keeps one line whole when several tasks log at once.
  flockfile(stdout);
  std::fprintf(stdout, "%c (%u) %s: ", kLetters[level], esp_log_timestamp(), tag);
  va_list args;
  va_start(args, format);
  std::vfprintf(stdout, format, args);
  va_end(args);
  std::fputc('\n', stdout);
  std::fflush(stdout);
  funlockfile(stdout);
}

uint32_t esp_random(void) {
  static std::random_device device;
  return device();
}

void* heap_caps_malloc(size_t size, uint32_t) { return std::malloc(size); }

void heap_caps_free(void* ptr) { std::free(ptr); }

size_t heap_caps_get_free_size(uint32_t) { return kReportedFreeBytes; }

// The project's version where it has one, as ESP-IDF finds it.
#ifndef PREFLASH_APP_VERSION
#define PREFLASH_APP_VERSION "simulator"
#endif

const esp_app_desc_t* esp_app_get_description(void) {
  static const esp_app_desc_t desc = [] {
    esp_app_desc_t d{};
    d.magic_word = ESP_APP_DESC_MAGIC_WORD;
    std::strncpy(d.version, PREFLASH_APP_VERSION, sizeof d.version - 1);
    std::strncpy(d.project_name, "preflash", sizeof d.project_name - 1);
    return d;
  }();
  return &desc;
}

esp_reset_reason_t esp_reset_reason(void) {
  return std::getenv(kRestartedEnv) ? ESP_RST_SW : ESP_RST_POWERON;
}

void esp_restart(void) {
  if (preflash::socket::quitting()) preflash::socket::quit();
  preflash::port::hold_input();
  // Not fflush(nullptr): that also waits on stdin, which the console task keeps
  // locked for as long as it sits in a read.
  std::fflush(stdout);
  std::fflush(stderr);
  char path[4096];
  uint32_t size = sizeof path;
  if (_NSGetExecutablePath(path, &size) != 0) {
    preflash_unsupported("esp_restart", "the executable's path is too long to re-run");
  }
  setenv(kRestartedEnv, "1", 1);
  preflash::carry_chips_across_restart();
  if (preflash::trace::on()) {
    setenv("PREFLASH_TRACE", "1", 1);
  } else {
    unsetenv("PREFLASH_TRACE");
  }
  // The stream's pipe ends with this process, and the next run needs the real fds.
  preflash::serial::hand_back();
  preflash::socket::announce_restart();
  preflash::serial::wait_sent();
  // Those waits take a while, and a quit that arrived meanwhile has been told yes.
  if (preflash::socket::quitting()) preflash::socket::quit();
  exec_clean(path, *_NSGetArgv());
  preflash_unsupported("esp_restart", "re-running the executable failed");
}

void esp_system_abort(const char* details) {
  std::fprintf(stderr, "abort() was called: %s\n", details);
  std::fflush(stderr);
  preflash::crash::note("system_abort", details);
  std::abort();
}

}  // extern "C"
