// What feeds the board, from the AXP2101, and the soft switch that turns it off.
#include "board.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace ui {
namespace {

constexpr int kBarHeight = 18;
constexpr int kLowBattery = 15;

lv_obj_t* g_bar = nullptr;
lv_obj_t* g_percent = nullptr;
lv_obj_t* g_chip = nullptr;
lv_obj_t* g_usb = nullptr;
lv_obj_t* g_charge = nullptr;

const char* charge_name(power::Charge charge) {
  switch (charge) {
    case power::Charge::none:
      return "idle";
    case power::Charge::charging:
      return "charging";
    case power::Charge::done:
      return "full";
    case power::Charge::discharging:
      return "on battery";
  }
  return "";
}

void turn_off(lv_event_t*) { power::turn_off(); }

void build(lv_obj_t* body) {
  if (!board::has_power_chip()) {
    lv_obj_t* box = card(body);
    caption(box, "This board has no power chip to read.");
    g_bar = nullptr;
    return;
  }
  lv_obj_t* battery = card(body);
  g_percent = row(battery, "Battery");
  g_bar = lv_bar_create(battery);
  lv_obj_set_size(g_bar, lv_pct(100), kBarHeight);
  lv_bar_set_range(g_bar, 0, 100);
  lv_obj_set_style_bg_color(g_bar, lv_color_hex(colour::kLine), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(g_bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(g_bar, lv_color_hex(colour::kAccent), LV_PART_INDICATOR);

  lv_obj_t* details = card(body);
  g_chip = row(details, "Power chip");
  g_usb = row(details, "USB");
  g_charge = row(details, "Charger");

  spacer(body);
  button(body, LV_SYMBOL_POWER "  Turn off", turn_off);
}

void refresh() {
  if (!g_bar) return;
  const power::Reading reading = monitor::latest().power;
  if (!reading.answered) {
    show(g_chip, Tone::warning, "no answer");
    show(g_percent, Tone::warning, "unknown");
    show(g_usb, Tone::warning, "unknown");
    show(g_charge, Tone::warning, "unknown");
    lv_bar_set_value(g_bar, 0, LV_ANIM_OFF);
    return;
  }
  const bool low = reading.battery && reading.percent <= kLowBattery;
  show(g_chip, Tone::plain, "ok");
  if (reading.battery) {
    show(g_percent, low ? Tone::warning : Tone::plain, "%d%%", reading.percent);
  } else {
    show(g_percent, Tone::plain, "none fitted");
  }
  show(g_usb, Tone::plain, "%s", reading.usb ? "plugged in" : "unplugged");
  show(g_charge, Tone::plain, "%s", charge_name(reading.charge));
  lv_bar_set_value(g_bar, reading.battery ? reading.percent : 0, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(g_bar, lv_color_hex(low ? colour::kAmber : colour::kAccent), LV_PART_INDICATOR);
}

}  // namespace

const Page kPower = {"power", "Power", build, refresh};

}  // namespace ui
