#include <cstdint>
#include <cstdio>

#include "header_only.h"
#include "sdkconfig.h"
// This component doesn't list thing in REQUIRES.
#include "thing.h"

extern const uint8_t hello_start[] asm("_binary_hello_txt_start");
extern const uint8_t hello_end[] asm("_binary_hello_txt_end");
extern const uint8_t notes_start[] asm("_binary_notes_txt_start");
extern const uint8_t notes_end[] asm("_binary_notes_txt_end");

extern "C" int lv_stand_in_os_none(void);
extern "C" int lv_stand_in_color_depth(void);
extern "C" int lv_stand_in_sdl(void);

extern "C" void app_main(void) {
  printf("hello from app_main\n");
  printf("CONFIG_THING_VALUE=%d\n", CONFIG_THING_VALUE);
  printf("CONFIG_THING_ENABLED=%d\n", CONFIG_THING_ENABLED);
  printf("CONFIG_THING_MASK=%#x\n", CONFIG_THING_MASK);
#ifdef CONFIG_THING_DISABLED
  printf("CONFIG_THING_DISABLED is defined\n");
#endif
  printf("CONFIG_IDF_TARGET=%s\n", CONFIG_IDF_TARGET);
  printf("thing=%d\n", thing_value());
  printf("header only=%d\n", HEADER_ONLY_VALUE);
  printf("embedded=%.*s\n", static_cast<int>(hello_end - hello_start), hello_start);
  // A text file ends in a NUL that counts towards its length, as on ESP-IDF.
  printf("embedded text=%s length=%d\n", notes_start, static_cast<int>(notes_end - notes_start));
  printf("lvgl os_none=%d color_depth=%d sdl=%d\n", lv_stand_in_os_none(),
         lv_stand_in_color_depth(), lv_stand_in_sdl());
#ifdef ESP_PLATFORM
  printf("ESP_PLATFORM is defined\n");
#endif
#ifdef NDEBUG
  printf("asserts are off\n");
#endif
}
