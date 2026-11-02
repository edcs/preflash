// The few parts every page is made of, and the palette they share.
#pragma once

#include <cstdarg>

#include "lvgl.h"

namespace ui {

namespace colour {
constexpr uint32_t kGround = 0x0e1113;
constexpr uint32_t kSurface = 0x15191c;
constexpr uint32_t kLine = 0x2a3036;
constexpr uint32_t kText = 0xe7ecef;
constexpr uint32_t kMuted = 0x8d98a1;
constexpr uint32_t kAccent = 0x5fd4e8;
constexpr uint32_t kAmber = 0xf2b84b;
constexpr uint32_t kBad = 0xff9a70;
}  // namespace colour

enum class Tone { plain, good, warning, bad };

uint32_t colour_of(Tone tone);

// A panel holding rows, stacked with a gap.
lv_obj_t* card(lv_obj_t* parent);

// A line of small muted text.
lv_obj_t* caption(lv_obj_t* parent, const char* text);

// A key on the left and a value on the right. Gives the value's label.
lv_obj_t* row(lv_obj_t* card, const char* key);

// Takes up the free height, so that what comes after it sits at the bottom of the page.
void spacer(lv_obj_t* parent);

// A button that fills its row.
lv_obj_t* button(lv_obj_t* parent, const char* text, lv_event_cb_t on_click);

// A labelled slider, 0 to 100.
lv_obj_t* slider(lv_obj_t* card, const char* key, int value, lv_event_cb_t on_release);

// Sets text and colour, redrawing only when they change.
void show(lv_obj_t* label, Tone tone, const char* format, ...) __attribute__((format(printf, 3, 4)));

}  // namespace ui
