#pragma once

#include <stdint.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;

#define ESP_OK 0
#define ESP_FAIL -1

#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_INVALID_CRC 0x109
#define ESP_ERR_INVALID_VERSION 0x10A
#define ESP_ERR_INVALID_MAC 0x10B
#define ESP_ERR_NOT_FINISHED 0x10C
#define ESP_ERR_NOT_ALLOWED 0x10D

#define ESP_ERR_WIFI_BASE 0x3000
#define ESP_ERR_MESH_BASE 0x4000
#define ESP_ERR_FLASH_BASE 0x6000
#define ESP_ERR_HW_CRYPTO_BASE 0xc000
#define ESP_ERR_MEMPROT_BASE 0xd000

const char* esp_err_to_name(esp_err_t code);

void _esp_error_check_failed(esp_err_t rc, const char* file, int line, const char* function,
                             const char* expression) __attribute__((noreturn));

// Always checked, unlike ESP-IDF's NDEBUG build: a hidden failure in the
// simulator would only move the crash somewhere harder to read.
#define ESP_ERROR_CHECK(x)                                                         \
  do {                                                                             \
    esp_err_t err_rc_ = (x);                                                       \
    if (__builtin_expect(err_rc_ != ESP_OK, 0)) {                                  \
      _esp_error_check_failed(err_rc_, __FILE__, __LINE__, __func__, #x);          \
    }                                                                              \
  } while (0)

#ifdef __cplusplus
}
#endif
