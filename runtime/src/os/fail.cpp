#include "preflash/fail.h"

#include <cstdio>
#include <cstdlib>

#include "esp_err.h"
#include "nvs.h"

[[noreturn]] void preflash_unsupported(const char* call, const char* why) {
  std::fprintf(stderr, "preflash: %s is not supported in the simulator: %s\n", call, why);
  std::fflush(stderr);
  std::abort();
}

extern "C" {

const char* esp_err_to_name(esp_err_t code) {
  switch (code) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_INVALID_SIZE: return "ESP_ERR_INVALID_SIZE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    case ESP_ERR_INVALID_RESPONSE: return "ESP_ERR_INVALID_RESPONSE";
    case ESP_ERR_INVALID_CRC: return "ESP_ERR_INVALID_CRC";
    case ESP_ERR_INVALID_VERSION: return "ESP_ERR_INVALID_VERSION";
    case ESP_ERR_INVALID_MAC: return "ESP_ERR_INVALID_MAC";
    case ESP_ERR_NOT_FINISHED: return "ESP_ERR_NOT_FINISHED";
    case ESP_ERR_NOT_ALLOWED: return "ESP_ERR_NOT_ALLOWED";
    case ESP_ERR_NVS_NOT_INITIALIZED: return "ESP_ERR_NVS_NOT_INITIALIZED";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_READ_ONLY: return "ESP_ERR_NVS_READ_ONLY";
    case ESP_ERR_NVS_INVALID_NAME: return "ESP_ERR_NVS_INVALID_NAME";
    case ESP_ERR_NVS_INVALID_HANDLE: return "ESP_ERR_NVS_INVALID_HANDLE";
    case ESP_ERR_NVS_KEY_TOO_LONG: return "ESP_ERR_NVS_KEY_TOO_LONG";
    case ESP_ERR_NVS_INVALID_LENGTH: return "ESP_ERR_NVS_INVALID_LENGTH";
    case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
    case ESP_ERR_NVS_NEW_VERSION_FOUND: return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    default: return "UNKNOWN ERROR";
  }
}

void _esp_error_check_failed(esp_err_t rc, const char* file, int line, const char* function,
                             const char* expression) {
  std::fprintf(stderr, "ESP_ERROR_CHECK failed: esp_err_t 0x%x (%s) at %s:%d, in %s\n  expression: %s\n",
               static_cast<unsigned>(rc), esp_err_to_name(rc), file, line, function, expression);
  std::fflush(stderr);
  std::abort();
}

}  // extern "C"
