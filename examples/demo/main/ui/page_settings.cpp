// What's saved in NVS. A change saves as the finger lifts, and survives a restart.
#include <algorithm>

#include "board.h"
#include "esp_system.h"
#include "settings.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace ui {
namespace {

constexpr int kLowestBrightness = 5;  // the slider must stay visible enough to find again

lv_obj_t* g_boots = nullptr;

void on_brightness(lv_event_t* event) {
  const int percent = std::max<int>(lv_slider_get_value(lv_event_get_target_obj(event)), kLowestBrightness);
  settings::set_brightness(percent);
  board::set_brightness(percent);
}

void on_restart(lv_event_t*) { esp_restart(); }

void build(lv_obj_t* body) {
  const settings::Values values = settings::get();
  lv_obj_t* box = card(body);
  slider(box, "Brightness", values.brightness, on_brightness);
  g_boots = row(box, "Boots");
  spacer(body);
  button(body, LV_SYMBOL_LOOP "  Restart", on_restart);
}

void refresh() {
  show(g_boots, Tone::plain, "%lu", static_cast<unsigned long>(settings::get().boots));
}

}  // namespace

const Page kSettings = {"settings", "Settings", build, refresh};

}  // namespace ui
