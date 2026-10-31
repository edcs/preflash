#pragma once

#include <stdbool.h>

#include "esp_codec_dev.h"
#include "esp_err.h"
#include "esp_io_expander.h"

// The speaker amplifier's switch, on the I/O expander.
#define BSP_POWER_AMP_IO (IO_EXPANDER_PIN_NUM_3)

#ifdef __cplusplus
extern "C" {
#endif

esp_io_expander_handle_t bsp_io_expander_init(void);
// The board package switches the amplifier on with the codec, so this does too.
esp_codec_dev_handle_t bsp_audio_codec_speaker_init(void);
esp_err_t bsp_audio_poweramp_enable(bool enable);

#ifdef __cplusplus
}
#endif
