#include "ui/ui.h"

#include <cstdio>
#include <cstring>

#include "board.h"
#include "ui/widgets.h"

namespace ui {
namespace {

constexpr int kRefreshMs = 200;
constexpr int kMargin = 16;
constexpr int kNarrowMargin = 8;
constexpr int kBackSize = 44;

const Page* const kPages[] = {&kHome,    &kTime,   &kMotion,   &kPower, &kSound,
                              &kNetwork, &kUpdate, &kSettings, &kFaults};

const Page* g_current = nullptr;

void go_home(lv_event_t*) { open(kHome); }

void refresh_page(lv_timer_t*) {
  if (g_current) g_current->refresh();
}

void add_title_bar(lv_obj_t* screen, const Page& page) {
  lv_obj_t* bar = lv_obj_create(screen);
  lv_obj_remove_style_all(bar);
  lv_obj_set_size(bar, lv_pct(100), kBackSize);
  lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(bar, 6, 0);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* back = lv_button_create(bar);
  lv_obj_set_size(back, kBackSize, kBackSize);
  lv_obj_set_style_radius(back, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(colour::kSurface), 0);
  lv_obj_set_style_bg_color(back, lv_color_hex(colour::kLine), LV_STATE_PRESSED);
  lv_obj_set_style_border_color(back, lv_color_hex(colour::kLine), 0);
  lv_obj_set_style_border_width(back, 1, 0);
  lv_obj_set_style_shadow_width(back, 0, 0);
  lv_obj_add_event_cb(back, go_home, LV_EVENT_CLICKED, nullptr);
  lv_obj_t* arrow = lv_label_create(back);
  lv_label_set_text(arrow, LV_SYMBOL_LEFT);
  lv_obj_set_style_text_color(arrow, lv_color_hex(colour::kAccent), 0);
  lv_obj_center(arrow);

  lv_obj_t* title = lv_label_create(bar);
  lv_label_set_text(title, page.title);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(colour::kText), 0);
}

}  // namespace

void open(const Page& page) {
  g_current = &page;
  lv_obj_t* screen = lv_screen_active();
  lv_obj_clean(screen);

  const int margin = narrow() ? kNarrowMargin : kMargin;
  lv_obj_set_style_pad_all(screen, margin, 0);
  lv_obj_set_style_pad_row(screen, margin, 0);
  lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
  if (&page != &kHome) add_title_bar(screen, page);

  lv_obj_t* body = lv_obj_create(screen);
  lv_obj_remove_style_all(body);
  lv_obj_set_width(body, lv_pct(100));
  lv_obj_set_flex_grow(body, 1);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, margin, 0);
  lv_obj_remove_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  page.build(body);
  page.refresh();
  std::printf("screen=%s\n", page.name);
}

bool open_by_name(const char* name) {
  for (const Page* page : kPages) {
    if (std::strcmp(page->name, name) == 0) {
      board::Lock lock;
      open(*page);
      return true;
    }
  }
  return false;
}

bool narrow() { return lv_display_get_physical_horizontal_resolution(lv_display_get_default()) < 300; }

void start() {
  board::Lock lock;
  lv_obj_t* screen = lv_screen_active();
  lv_obj_set_style_bg_color(screen, lv_color_hex(colour::kGround), 0);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
  lv_obj_set_style_text_font(screen, &lv_font_montserrat_16, 0);
  lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  open(kHome);
  lv_timer_create(refresh_page, kRefreshMs, nullptr);
}

const char* describe(rtc::Status status) {
  switch (status) {
    case rtc::Status::ok:
      return "ok";
    case rtc::Status::no_answer:
      return "no answer";
    case rtc::Status::time_lost:
      return "time lost";
  }
  return "";
}

const char* describe(network::Wifi wifi) {
  switch (wifi) {
    case network::Wifi::off:
      return "off";
    case network::Wifi::joining:
      return "joining";
    case network::Wifi::joined:
      return "joined";
    case network::Wifi::failed:
      return "failed";
  }
  return "";
}

const char* describe(network::Fetch fetch) {
  switch (fetch) {
    case network::Fetch::idle:
      return "idle";
    case network::Fetch::running:
      return "fetching";
    case network::Fetch::ok:
      return "ok";
    case network::Fetch::failed:
      return "failed";
  }
  return "";
}

const char* describe(update::Check check) {
  switch (check) {
    case update::Check::idle:
      return "not checked";
    case update::Check::running:
      return "checking";
    case update::Check::current:
      return "up to date";
    case update::Check::newer:
      return "newer version";
    case update::Check::failed:
      return "couldn't check";
  }
  return "";
}

}  // namespace ui
