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
#include "preflash/fail.h"

namespace {

// Set across a restart so the next run can report a software reset.
constexpr const char* kRestartedEnv = "PREFLASH_RESTARTED";

constexpr size_t kReportedFreeBytes = 256 * 1024;

// Replaces this process with a fresh run of the executable. Plain execv would
// pass on every open descriptor, the calling thread's blocked signals and any
// ignored ones, so this closes every descriptor but the standard three and
// resets the signals in the same step, whichever thread opened what. Returns
// only on failure.
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
  int err = posix_spawnattr_setsigmask(&attr, &none);
  if (!err) err = posix_spawnattr_setsigdefault(&attr, &all);
  if (!err) {
    err = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_CLOEXEC_DEFAULT |
                                              POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);
  }
  for (int fd = STDIN_FILENO; !err && fd <= STDERR_FILENO; ++fd) {
    // Inheriting a closed descriptor would fail the whole spawn.
    if (fcntl(fd, F_GETFD) != -1) err = posix_spawn_file_actions_addinherit_np(&actions, fd);
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

const esp_app_desc_t* esp_app_get_description(void) {
  static const esp_app_desc_t desc = [] {
    esp_app_desc_t d{};
    d.magic_word = ESP_APP_DESC_MAGIC_WORD;
    std::strncpy(d.version, "simulator", sizeof d.version - 1);
    std::strncpy(d.project_name, "preflash", sizeof d.project_name - 1);
    return d;
  }();
  return &desc;
}

esp_reset_reason_t esp_reset_reason(void) {
  return std::getenv(kRestartedEnv) ? ESP_RST_SW : ESP_RST_POWERON;
}

void esp_restart(void) {
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
  exec_clean(path, *_NSGetArgv());
  preflash_unsupported("esp_restart", "re-running the executable failed");
}

}  // extern "C"
