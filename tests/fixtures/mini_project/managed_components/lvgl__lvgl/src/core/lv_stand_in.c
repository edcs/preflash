// Reports the config LVGL's sources would see, without a full copy of LVGL here.
#ifndef LV_CONF_KCONFIG_EXTERNAL_INCLUDE
#error "LVGL needs its config header"
#endif
#include LV_CONF_KCONFIG_EXTERNAL_INCLUDE

#ifdef CONFIG_LV_OS_FREERTOS
#error "the host has no FreeRTOS for LVGL"
#endif

int lv_stand_in_os_threads(void) {
#ifdef CONFIG_LV_OS_PTHREAD
  return 1;
#else
  return 0;
#endif
}

int lv_stand_in_os_none(void) {
#ifdef CONFIG_LV_OS_NONE
  return 1;
#else
  return 0;
#endif
}

int lv_stand_in_color_depth(void) { return CONFIG_LV_COLOR_DEPTH; }

int lv_stand_in_sdl(void) {
#ifdef CONFIG_LV_USE_SDL
  return 1;
#else
  return 0;
#endif
}
