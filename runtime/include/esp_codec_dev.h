#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_CODEC_DEV_OK (0)
#define ESP_CODEC_DEV_DRV_ERR (ESP_FAIL)
#define ESP_CODEC_DEV_INVALID_ARG (ESP_ERR_INVALID_ARG)
#define ESP_CODEC_DEV_WRONG_STATE (ESP_ERR_INVALID_STATE)

typedef void* esp_codec_dev_handle_t;

typedef struct {
  uint8_t bits_per_sample;
  uint8_t channel;
  uint16_t channel_mask;
  uint32_t sample_rate;
  int mclk_multiple;
} esp_codec_dev_sample_info_t;

int esp_codec_dev_open(esp_codec_dev_handle_t codec, esp_codec_dev_sample_info_t* fs);
int esp_codec_dev_close(esp_codec_dev_handle_t codec);
int esp_codec_dev_write(esp_codec_dev_handle_t codec, void* data, int len);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t codec, int volume);

#ifdef __cplusplus
}
#endif
