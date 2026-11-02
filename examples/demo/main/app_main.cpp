// A small bench instrument: it reads the board's chips, shows them on pages reached by touch,
// and keeps going when any part of the hardware or network fails. See ../README.md.
#include "audio.h"
#include "board.h"
#include "console.h"
#include "esp_log.h"
#include "monitor.h"
#include "network.h"
#include "power.h"
#include "rtc.h"
#include "settings.h"
#include "timekeeper.h"
#include "ui/ui.h"
#include "update.h"

namespace {

constexpr const char* kTag = "demo";

}  // namespace

extern "C" void app_main(void) {
  settings::start();
  board::start();

  rtc::start(board::sensor_bus());
  motion::start(board::sensor_bus());
  if (board::has_power_chip()) power::start(board::sensor_bus());
  timekeeper::start();
  monitor::start();

  audio::start();
  update::start();
  ui::start();
  board::set_brightness(settings::get().brightness);

  console::start();
  network::start();
  ESP_LOGI(kTag, "ready on %s", board::name());
}
