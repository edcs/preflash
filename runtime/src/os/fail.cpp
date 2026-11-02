#include "preflash/fail.h"

#include <cstdio>
#include <cstdlib>

#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "preflash/crash.h"
#include "preflash/serial.h"

[[noreturn]] void preflash_unsupported(const char* call, const char* why) {
  std::fprintf(stderr, "preflash: %s is not supported in the simulator: %s\n", call, why);
  preflash::serial::flush_stdio();
  char message[512];
  std::snprintf(message, sizeof message, "%s is not supported in the simulator: %s", call, why);
  preflash::crash::note("unsupported", message);
  // The handler for SIGABRT drains the stream.
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
    case ESP_ERR_WIFI_NOT_INIT: return "ESP_ERR_WIFI_NOT_INIT";
    case ESP_ERR_WIFI_NOT_STARTED: return "ESP_ERR_WIFI_NOT_STARTED";
    case ESP_ERR_WIFI_CONN: return "ESP_ERR_WIFI_CONN";
    case ESP_ERR_WIFI_SSID: return "ESP_ERR_WIFI_SSID";
    case ESP_ERR_WIFI_PASSWORD: return "ESP_ERR_WIFI_PASSWORD";
    case ESP_ERR_WIFI_TIMEOUT: return "ESP_ERR_WIFI_TIMEOUT";
    case ESP_ERR_WIFI_NOT_CONNECT: return "ESP_ERR_WIFI_NOT_CONNECT";
    case ESP_ERR_HTTP_MAX_REDIRECT: return "ESP_ERR_HTTP_MAX_REDIRECT";
    case ESP_ERR_HTTP_CONNECT: return "ESP_ERR_HTTP_CONNECT";
    case ESP_ERR_HTTP_WRITE_DATA: return "ESP_ERR_HTTP_WRITE_DATA";
    case ESP_ERR_HTTP_FETCH_HEADER: return "ESP_ERR_HTTP_FETCH_HEADER";
    case ESP_ERR_HTTP_INVALID_TRANSPORT: return "ESP_ERR_HTTP_INVALID_TRANSPORT";
    case ESP_ERR_HTTP_CONNECTING: return "ESP_ERR_HTTP_CONNECTING";
    case ESP_ERR_HTTP_EAGAIN: return "ESP_ERR_HTTP_EAGAIN";
    case ESP_ERR_HTTP_CONNECTION_CLOSED: return "ESP_ERR_HTTP_CONNECTION_CLOSED";
    default: return "UNKNOWN ERROR";
  }
}

void _esp_error_check_failed(esp_err_t rc, const char* file, int line, const char* function,
                             const char* expression) {
  std::fprintf(stderr, "ESP_ERROR_CHECK failed: esp_err_t 0x%x (%s) at %p\n",
               static_cast<unsigned>(rc), esp_err_to_name(rc), __builtin_return_address(0));
  std::fprintf(stderr, "file: \"%s\" line %d\nfunc: %s\nexpression: %s\n", file, line, function,
               expression);
  std::fflush(stderr);
  preflash::crash::note("error_check", esp_err_to_name(rc));
  std::abort();
}

}  // extern "C"
