#include "ui/widgets.h"

#include <cstdio>
#include <cstring>

#include "ui/ui.h"

namespace ui {
namespace {

constexpr int kCardPad = 12;
constexpr int kNarrowCardPad = 8;
constexpr int kRadius = 10;
constexpr int kButtonHeight = 48;

const lv_font_t* small_font() { return narrow() ? &lv_font_montserrat_12 : &lv_font_montserrat_14; }

const lv_font_t* body_font() { return narrow() ? &lv_font_montserrat_14 : &lv_font_montserrat_16; }

}  // namespace

uint32_t colour_of(Tone tone) {
  switch (tone) {
    case Tone::good:
      return colour::kAccent;
    case Tone::warning:
      return colour::kAmber;
    case Tone::bad:
      return colour::kBad;
    case Tone::plain:
      break;
  }
  return colour::kText;
}

lv_obj_t* card(lv_obj_t* parent) {
  lv_obj_t* box = lv_obj_create(parent);
  lv_obj_remove_style_all(box);
  lv_obj_set_width(box, lv_pct(100));
  lv_obj_set_height(box, LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_bg_color(box, lv_color_hex(colour::kSurface), 0);
  lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(box, lv_color_hex(colour::kLine), 0);
  lv_obj_set_style_border_width(box, 1, 0);
  lv_obj_set_style_radius(box, kRadius, 0);
  lv_obj_set_style_pad_all(box, narrow() ? kNarrowCardPad : kCardPad, 0);
  lv_obj_set_style_pad_row(box, narrow() ? 6 : 10, 0);
  lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
  return box;
}

lv_obj_t* caption(lv_obj_t* parent, const char* text) {
  lv_obj_t* label = lv_label_create(parent);
  lv_label_set_text(label, text);
  lv_obj_set_width(label, lv_pct(100));
  lv_obj_set_style_text_color(label, lv_color_hex(colour::kMuted), 0);
  lv_obj_set_style_text_font(label, small_font(), 0);
  return label;
}

lv_obj_t* row(lv_obj_t* parent, const char* key) {
  lv_obj_t* line = lv_obj_create(parent);
  lv_obj_remove_style_all(line);
  lv_obj_set_size(line, lv_pct(100), LV_SIZE_CONTENT);
  // On the narrow panel a long value won't fit beside its key, so it goes underneath.
  lv_obj_set_flex_flow(line, narrow() ? LV_FLEX_FLOW_COLUMN : LV_FLEX_FLOW_ROW);
  if (!narrow()) {
    lv_obj_set_flex_align(line, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  }
  lv_obj_remove_flag(line, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t* name = lv_label_create(line);
  lv_label_set_text(name, key);
  lv_obj_set_style_text_color(name, lv_color_hex(colour::kMuted), 0);
  lv_obj_set_style_text_font(name, small_font(), 0);

  lv_obj_t* value = lv_label_create(line);
  lv_label_set_text(value, "");
  lv_obj_set_style_text_color(value, lv_color_hex(colour::kText), 0);
  lv_obj_set_style_text_font(value, body_font(), 0);
  return value;
}

void spacer(lv_obj_t* parent) {
  lv_obj_t* gap = lv_obj_create(parent);
  lv_obj_remove_style_all(gap);
  lv_obj_set_width(gap, lv_pct(100));
  lv_obj_set_flex_grow(gap, 1);
}

lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t on_click) {
  lv_obj_t* box = lv_button_create(parent);
  lv_obj_set_size(box, lv_pct(100), kButtonHeight);
  lv_obj_set_style_radius(box, kRadius, 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(colour::kLine), 0);
  lv_obj_set_style_bg_color(box, lv_color_hex(colour::kAccent), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(box, 0, 0);
  lv_obj_add_event_cb(box, on_click, LV_EVENT_CLICKED, nullptr);

  lv_obj_t* label = lv_label_create(box);
  lv_label_set_text(label, text);
  lv_obj_set_style_text_color(label, lv_color_hex(colour::kText), 0);
  lv_obj_set_style_text_font(label, body_font(), 0);
  lv_obj_center(label);
  return box;
}

lv_obj_t* slider(lv_obj_t* parent, const char* key, int value, lv_event_cb_t on_release) {
  caption(parent, key);
  lv_obj_t* bar = lv_slider_create(parent);
  lv_obj_set_width(bar, lv_pct(92));
  lv_obj_set_style_align(bar, LV_ALIGN_CENTER, 0);
  lv_slider_set_range(bar, 0, 100);
  lv_slider_set_value(bar, value, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(bar, lv_color_hex(colour::kLine), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_bg_color(bar, lv_color_hex(colour::kAccent), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar, lv_color_hex(colour::kText), LV_PART_KNOB);
  lv_obj_add_event_cb(bar, on_release, LV_EVENT_RELEASED, nullptr);
  return bar;
}

void show(lv_obj_t* label, Tone tone, const char* format, ...) {
  char text[64];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof text, format, args);
  va_end(args);
  if (std::strcmp(lv_label_get_text(label), text) != 0) lv_label_set_text(label, text);
  const lv_color_t colour = lv_color_hex(colour_of(tone));
  if (!lv_color_eq(lv_obj_get_style_text_color(label, LV_PART_MAIN), colour)) {
    lv_obj_set_style_text_color(label, colour, 0);
  }
}

}  // namespace ui
