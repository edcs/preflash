// The clock inside a turning ring, and a tile for each page with its reading on it.
#include <cstdio>
#include <ctime>

#include "audio.h"
#include "board.h"
#include "settings.h"
#include "timekeeper.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace ui {
namespace {

constexpr int kRingSize = 176;
constexpr int kNarrowRingSize = 150;
constexpr int kRingWidth = 6;
constexpr int kRingArc = 70;
constexpr int kRingAtRest = 310;  // where the turning part sits while the clock is held
constexpr int kLapMs = 4000;
constexpr int kTileGap = 8;
constexpr int kIconGap = 2;
constexpr int kLowBattery = 15;

enum TileIndex {
  kTimeTile,
  kMotionTile,
  kPowerTile,
  kSoundTile,
  kNetworkTile,
  kUpdateTile,
  kSettingsTile,
  kFaultsTile,
};

struct Tile {
  const char* symbol;
  const char* label;
  const Page* page;
  lv_obj_t* value;
};

Tile g_tiles[] = {
    {LV_SYMBOL_REFRESH, "Time", &kTime, nullptr},       {LV_SYMBOL_GPS, "Motion", &kMotion, nullptr},
    {LV_SYMBOL_BATTERY_3, "Power", &kPower, nullptr},   {LV_SYMBOL_VOLUME_MAX, "Sound", &kSound, nullptr},
    {LV_SYMBOL_WIFI, "Network", &kNetwork, nullptr},    {LV_SYMBOL_DOWNLOAD, "Update", &kUpdate, nullptr},
    {LV_SYMBOL_SETTINGS, "Settings", &kSettings, nullptr}, {LV_SYMBOL_WARNING, "Faults", &kFaults, nullptr},
};

lv_obj_t* g_ring = nullptr;
lv_obj_t* g_time = nullptr;
lv_obj_t* g_date = nullptr;

void set_ring(void* ring, int32_t angle) {
  lv_arc_set_angles(static_cast<lv_obj_t*>(ring), angle, angle + kRingArc);
}

void start_turning() {
  lv_anim_t lap;
  lv_anim_init(&lap);
  lv_anim_set_var(&lap, g_ring);
  lv_anim_set_exec_cb(&lap, set_ring);
  lv_anim_set_values(&lap, 0, 360);
  lv_anim_set_duration(&lap, kLapMs);
  lv_anim_set_repeat_count(&lap, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_path_cb(&lap, lv_anim_path_linear);
  lv_anim_start(&lap);
}

// Starts or stops the ring when the clock is released or held.
void follow_hold() {
  const bool turning = lv_anim_get(g_ring, set_ring) != nullptr;
  const bool held = timekeeper::held();
  if (held && turning) {
    lv_anim_delete(g_ring, set_ring);
    set_ring(g_ring, kRingAtRest);
  } else if (!held && !turning) {
    start_turning();
  }
}

void add_ring(lv_obj_t* body) {
  const int size = narrow() ? kNarrowRingSize : kRingSize;
  g_ring = lv_arc_create(body);
  lv_obj_set_size(g_ring, size, size);
  lv_obj_set_style_align(g_ring, LV_ALIGN_CENTER, 0);
  lv_obj_remove_flag(g_ring, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_style(g_ring, nullptr, LV_PART_KNOB);
  lv_arc_set_bg_angles(g_ring, 0, 360);
  lv_obj_set_style_arc_width(g_ring, kRingWidth, LV_PART_MAIN);
  lv_obj_set_style_arc_color(g_ring, lv_color_hex(colour::kLine), LV_PART_MAIN);
  lv_obj_set_style_arc_width(g_ring, kRingWidth, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(g_ring, lv_color_hex(colour::kAccent), LV_PART_INDICATOR);
  set_ring(g_ring, kRingAtRest);

  g_time = lv_label_create(g_ring);
  lv_obj_set_style_text_font(g_time, narrow() ? &lv_font_montserrat_40 : &lv_font_montserrat_48, 0);
  lv_obj_set_style_text_color(g_time, lv_color_hex(colour::kText), 0);
  lv_label_set_text(g_time, "");
  lv_obj_align(g_time, LV_ALIGN_CENTER, 0, -8);

  g_date = lv_label_create(g_ring);
  lv_obj_set_style_text_font(g_date, narrow() ? &lv_font_montserrat_12 : &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(g_date, lv_color_hex(colour::kMuted), 0);
  lv_label_set_text(g_date, "");
  lv_obj_align(g_date, LV_ALIGN_CENTER, 0, 26);
}

void open_tile(lv_event_t* event) {
  open(*static_cast<const Tile*>(lv_event_get_user_data(event))->page);
}

void add_tile(lv_obj_t* grid, Tile& tile) {
  const int columns = narrow() ? 2 : 4;
  lv_obj_t* box = lv_obj_create(grid);
  lv_obj_remove_style_all(box);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(box, kIconGap, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(colour::kSurface), 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(colour::kLine), LV_STATE_PRESSED);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(colour::kLine), 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_radius(box, 10, 0);
  const int32_t grid_width = lv_obj_get_content_width(grid);
  lv_obj_set_size(box, (grid_width - (columns - 1) * kTileGap) / columns, narrow() ? 96 : 108);
  lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_add_event_cb(box, open_tile, LV_EVENT_CLICKED, &tile);

  lv_obj_t* icon = lv_label_create(box);
  lv_label_set_text(icon, tile.symbol);
  lv_obj_set_style_text_font(icon, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(icon, lv_color_hex(colour::kAccent), 0);

  lv_obj_t* name = lv_label_create(box);
  lv_label_set_text(name, tile.label);
  lv_obj_set_style_text_font(name, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(name, lv_color_hex(colour::kText), 0);

  tile.value = lv_label_create(box);
  lv_label_set_text(tile.value, "");
  lv_obj_set_style_text_font(tile.value, &lv_font_montserrat_12, 0);
}

void build(lv_obj_t* body) {
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  add_ring(body);

  lv_obj_t* grid = lv_obj_create(body);
  lv_obj_remove_style_all(grid);
  lv_obj_set_width(grid, lv_pct(100));
  lv_obj_set_height(grid, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
  lv_obj_set_style_pad_row(grid, kTileGap, 0);
  lv_obj_set_style_pad_column(grid, kTileGap, 0);
  lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_update_layout(lv_screen_active());
  for (Tile& tile : g_tiles) add_tile(grid, tile);
}

void show_clock() {
  if (!timekeeper::known()) {
    show(g_time, Tone::warning, "--:--");
    show(g_date, Tone::warning, "time unknown");
    return;
  }
  const time_t utc = timekeeper::now();
  std::tm date;
  gmtime_r(&utc, &date);
  char clock[8];
  char day[20];
  std::strftime(clock, sizeof clock, "%H:%M", &date);
  std::strftime(day, sizeof day, "%a %d %b", &date);
  show(g_time, Tone::plain, "%s", clock);
  show(g_date, Tone::plain, "%s", day);
}

void show_power_tile(lv_obj_t* value, const power::Reading& reading) {
  if (!board::has_power_chip()) {
    show(value, Tone::plain, "no chip");
  } else if (!reading.answered) {
    show(value, Tone::warning, "no answer");
  } else if (!reading.battery) {
    show(value, Tone::plain, "USB");
  } else {
    show(value, reading.percent <= kLowBattery ? Tone::warning : Tone::plain, "%d%%", reading.percent);
  }
}

void refresh() {
  follow_hold();
  show_clock();

  const monitor::Snapshot chips = monitor::latest();
  const network::Status net = network::status();
  const bool rtc_ok = chips.rtc == rtc::Status::ok;
  show(g_tiles[kTimeTile].value, rtc_ok ? Tone::plain : Tone::warning, "%s", rtc_ok ? "RTC ok" : describe(chips.rtc));
  if (chips.motion.answered) {
    show(g_tiles[kMotionTile].value, Tone::plain, "%d\xc2\xb0", chips.motion.degrees);
  } else {
    show(g_tiles[kMotionTile].value, Tone::warning, "no answer");
  }
  show_power_tile(g_tiles[kPowerTile].value, chips.power);
  show(g_tiles[kSoundTile].value, Tone::plain, audio::available() ? "ready" : "no codec");
  show(g_tiles[kNetworkTile].value, net.wifi == network::Wifi::failed ? Tone::warning : Tone::plain, "%s",
       describe(net.wifi));
  show(g_tiles[kUpdateTile].value, Tone::plain, "v%s", update::status().version);
  show(g_tiles[kSettingsTile].value, Tone::plain, "boot %lu", static_cast<unsigned long>(settings::get().boots));
  const int problems = faults_showing();
  if (problems == 0) {
    show(g_tiles[kFaultsTile].value, Tone::good, "all clear");
  } else {
    show(g_tiles[kFaultsTile].value, Tone::bad, "%d showing", problems);
  }
}

}  // namespace

const Page kHome = {"home", "Home", build, refresh};

}  // namespace ui
