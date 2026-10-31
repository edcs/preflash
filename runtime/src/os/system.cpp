#include <crt_externs.h>
#include <mach-o/dyld.h>
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
  std::fflush(nullptr);
  char path[4096];
  uint32_t size = sizeof path;
  if (_NSGetExecutablePath(path, &size) != 0) {
    preflash_unsupported("esp_restart", "the executable's path is too long to re-run");
  }
  setenv(kRestartedEnv, "1", 1);
  execv(path, *_NSGetArgv());
  preflash_unsupported("esp_restart", "re-running the executable failed");
}

}  // extern "C"
