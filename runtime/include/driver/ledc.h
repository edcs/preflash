#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  LEDC_LOW_SPEED_MODE,
  LEDC_SPEED_MODE_MAX,
} ledc_mode_t;

typedef enum {
  LEDC_INTR_DISABLE = 0,
  LEDC_INTR_FADE_END,
  LEDC_INTR_MAX,
} ledc_intr_type_t;

typedef enum {
  LEDC_AUTO_CLK = 0,
  LEDC_USE_APB_CLK,
  LEDC_USE_RC_FAST_CLK,
  LEDC_USE_XTAL_CLK,
} ledc_clk_cfg_t;

typedef enum {
  LEDC_TIMER_0 = 0,
  LEDC_TIMER_1,
  LEDC_TIMER_2,
  LEDC_TIMER_3,
  LEDC_TIMER_MAX,
} ledc_timer_t;

typedef enum {
  LEDC_CHANNEL_0 = 0,
  LEDC_CHANNEL_1,
  LEDC_CHANNEL_2,
  LEDC_CHANNEL_3,
  LEDC_CHANNEL_4,
  LEDC_CHANNEL_5,
  LEDC_CHANNEL_6,
  LEDC_CHANNEL_7,
  LEDC_CHANNEL_MAX,
} ledc_channel_t;

typedef enum {
  LEDC_TIMER_1_BIT = 1,
  LEDC_TIMER_2_BIT,
  LEDC_TIMER_3_BIT,
  LEDC_TIMER_4_BIT,
  LEDC_TIMER_5_BIT,
  LEDC_TIMER_6_BIT,
  LEDC_TIMER_7_BIT,
  LEDC_TIMER_8_BIT,
  LEDC_TIMER_9_BIT,
  LEDC_TIMER_10_BIT,
  LEDC_TIMER_11_BIT,
  LEDC_TIMER_12_BIT,
  LEDC_TIMER_13_BIT,
  LEDC_TIMER_14_BIT,
  LEDC_TIMER_BIT_MAX,
} ledc_timer_bit_t;

typedef enum {
  LEDC_SLEEP_MODE_NO_ALIVE_NO_PD = 0,
  LEDC_SLEEP_MODE_NO_ALIVE_ALLOW_PD,
  LEDC_SLEEP_MODE_KEEP_ALIVE,
  LEDC_SLEEP_MODE_INVALID,
} ledc_sleep_mode_t;

typedef struct {
  int gpio_num;
  ledc_mode_t speed_mode;
  ledc_channel_t channel;
  ledc_intr_type_t intr_type;
  ledc_timer_t timer_sel;
  uint32_t duty;
  int hpoint;
  ledc_sleep_mode_t sleep_mode;
  struct {
    unsigned int output_invert : 1;
  } flags;
} ledc_channel_config_t;

typedef struct {
  ledc_mode_t speed_mode;
  ledc_timer_bit_t duty_resolution;
  ledc_timer_t timer_num;
  uint32_t freq_hz;
  ledc_clk_cfg_t clk_cfg;
  bool deconfigure;
} ledc_timer_config_t;

// A channel on the board's backlight pin sets the panel's brightness. Other pins keep
// their duty and drive nothing. Fades aren't simulated, so a project using them stops at
// the build.
esp_err_t ledc_timer_config(const ledc_timer_config_t* timer_conf);
esp_err_t ledc_channel_config(const ledc_channel_config_t* ledc_conf);
esp_err_t ledc_set_duty(ledc_mode_t speed_mode, ledc_channel_t channel, uint32_t duty);
esp_err_t ledc_update_duty(ledc_mode_t speed_mode, ledc_channel_t channel);

#ifdef __cplusplus
}
#endif
