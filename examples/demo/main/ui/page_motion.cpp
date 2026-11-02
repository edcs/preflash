// A spirit level: the dot sits where gravity pulls it, and the screen turns to stay upright.
#include "ui/ui.h"
#include "ui/widgets.h"

namespace ui {
namespace {

constexpr int kLevelSize = 180;
constexpr int kNarrowLevelSize = 140;
constexpr int kDotSize = 24;
constexpr int kCrossThickness = 1;
constexpr int kFullScaleMg = 1000;

lv_obj_t* g_level = nullptr;
lv_obj_t* g_dot = nullptr;
lv_obj_t* g_x = nullptr;
lv_obj_t* g_y = nullptr;
lv_obj_t* g_turned = nullptr;
lv_obj_t* g_screen = nullptr;

void add_hairline(lv_obj_t* level, int width, int height) {
  lv_obj_t* line = lv_obj_create(level);
  lv_obj_remove_style_all(line);
  lv_obj_set_size(line, width, height);
  lv_obj_set_style_bg_color(line, lv_color_hex(colour::kLine), 0);
  lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
  lv_obj_center(line);
}

void build(lv_obj_t* body) {
  lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  const int size = narrow() ? kNarrowLevelSize : kLevelSize;
  g_level = lv_obj_create(body);
  lv_obj_remove_style_all(g_level);
  lv_obj_set_size(g_level, size, size);
  lv_obj_set_style_bg_color(g_level, lv_color_hex(colour::kSurface), 0);
  lv_obj_set_style_bg_opa(g_level, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(g_level, lv_color_hex(colour::kLine), 0);
  lv_obj_set_style_border_width(g_level, 1, 0);
  lv_obj_set_style_radius(g_level, LV_RADIUS_CIRCLE, 0);
  add_hairline(g_level, size, kCrossThickness);
  add_hairline(g_level, kCrossThickness, size);

  g_dot = lv_obj_create(g_level);
  lv_obj_remove_style_all(g_dot);
  lv_obj_set_size(g_dot, kDotSize, kDotSize);
  lv_obj_set_style_radius(g_dot, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(g_dot, lv_color_hex(colour::kAccent), 0);
  lv_obj_set_style_bg_opa(g_dot, LV_OPA_COVER, 0);
  lv_obj_center(g_dot);

  lv_obj_t* details = card(body);
  g_x = row(details, "Gravity X");
  g_y = row(details, "Gravity Y");
  g_turned = row(details, "Board turned");
  g_screen = row(details, "Screen follows");
}

void refresh() {
  const monitor::Snapshot chips = monitor::latest();
  const motion::Reading& reading = chips.motion;
  if (!reading.answered) {
    lv_obj_add_flag(g_dot, LV_OBJ_FLAG_HIDDEN);
    show(g_x, Tone::warning, "no answer");
    show(g_y, Tone::warning, "no answer");
    show(g_turned, Tone::warning, "unknown");
    show(g_screen, Tone::plain, "%d\xc2\xb0", chips.shown_degrees);
    return;
  }
  lv_obj_remove_flag(g_dot, LV_OBJ_FLAG_HIDDEN);
  const int reach = (lv_obj_get_width(g_level) - kDotSize) / 2;
  lv_obj_align(g_dot, LV_ALIGN_CENTER, reading.x_mg * reach / kFullScaleMg, reading.y_mg * reach / kFullScaleMg);
  show(g_x, Tone::plain, "%d mg", reading.x_mg);
  show(g_y, Tone::plain, "%d mg", reading.y_mg);
  show(g_turned, Tone::plain, "%d\xc2\xb0", reading.degrees);
  show(g_screen, Tone::plain, "%d\xc2\xb0", chips.shown_degrees);
}

}  // namespace

const Page kMotion = {"motion", "Motion", build, refresh};

}  // namespace ui
