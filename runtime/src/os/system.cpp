#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "preflash/crash.h"
#include "preflash/platform.h"
#include "preflash/port.h"
#include "preflash/power.h"

namespace {

constexpr size_t kReportedFreeBytes = 256 * 1024;

}  // namespace

namespace preflash {

// Plain execve would pass on every open descriptor, the calling thread's blocked signals and
// any ignored ones, so this closes every descriptor but the standard three and the virtual
// serial port, and resets the signals in the same step, whichever thread opened what.
int exec_clean(const char* path, char* const argv[], char* const envp[]) {
#ifndef __APPLE__
  // Linux has no spawn that replaces the process, so this does by hand what the flags below
  // do. Every signal waits while the dispositions and descriptors change, and the new run
  // starts with none blocked. A caught signal that arrived meanwhile runs its handler as the
  // mask clears, before the exec, as it would have in an app that wasn't restarting.
  sigset_t all;
  sigset_t none;
  sigset_t previous;
  sigfillset(&all);
  sigemptyset(&none);
  pthread_sigmask(SIG_SETMASK, &all, &previous);
  for (int number = 1; number < NSIG; ++number) {
    // main keeps these ignored only if they arrived that way, as under nohup, and the next
    // run must inherit that, unlike any other ignored signal. The exec resets caught ones.
    // SIGPIPE waits for last: other threads still run and only this one blocks signals, so a
    // write to a dead pipe in the meantime would end the app mid-restart.
    if (number == SIGKILL || number == SIGSTOP || number == SIGTERM || number == SIGHUP ||
        number == SIGPIPE) {
      continue;
    }
    struct sigaction current {};
    if (sigaction(number, nullptr, &current) == 0 && current.sa_handler == SIG_IGN) {
      signal(number, SIG_DFL);
    }
  }
  // The standard three stay open as macOS keeps them below, even if something marked them to
  // close on exec.
  const int keep[] = {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO, preflash::port::master_fd(),
                      preflash::port::slave_fd()};
  struct sigaction pipe_before {};
  bool pipe_reset = false;
  if (preflash::platform::close_on_exec_except(keep, 5)) {
    if (sigaction(SIGPIPE, nullptr, &pipe_before) == 0 && pipe_before.sa_handler == SIG_IGN) {
      signal(SIGPIPE, SIG_DFL);
      pipe_reset = true;
    }
    pthread_sigmask(SIG_SETMASK, &none, nullptr);
    execve(path, argv, envp);
  }
  // Only a failed exec gets here, and the app goes on as it was.
  if (pipe_reset) sigaction(SIGPIPE, &pipe_before, nullptr);
  pthread_sigmask(SIG_SETMASK, &previous, nullptr);
  return -1;
#else
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
  if (!err) err = posix_spawn(nullptr, path, &actions, &attr, argv, envp);
  posix_spawn_file_actions_destroy(&actions);
  posix_spawnattr_destroy(&attr);
  return err ? -1 : 0;
#endif
}

}  // namespace preflash

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
  const char* reason = std::getenv("PREFLASH_RESET");
  return reason && std::strcmp(reason, "sw") == 0 ? ESP_RST_SW : ESP_RST_POWERON;
}

void esp_restart(void) {
  preflash::power::enter(preflash::power::Mode::On, preflash::power::Reset::Software);
}

void esp_system_abort(const char* details) {
  std::fprintf(stderr, "abort() was called: %s\n", details);
  std::fflush(stderr);
  preflash::crash::note("system_abort", details);
  std::abort();
}

}  // extern "C"
