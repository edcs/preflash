// A tone through the codec, at the saved volume.
#include "audio.h"
#include "settings.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace ui {
namespace {

lv_obj_t* g_frames = nullptr;

void on_beep(lv_event_t*) { audio::beep(); }

void on_volume(lv_event_t* event) {
  const int percent = lv_slider_get_value(lv_event_get_target_obj(event));
  settings::set_volume(percent);
  audio::set_volume(percent);
}

void build(lv_obj_t* body) {
  g_frames = nullptr;
  if (!audio::available()) {
    caption(card(body), "This board plays no sound in this demo.");
    return;
  }
  lv_obj_t* box = card(body);
  slider(box, "Volume", settings::get().volume, on_volume);
  g_frames = row(box, "Samples played");
  spacer(body);
  button(body, LV_SYMBOL_PLAY "  Play tone", on_beep);
}

void refresh() {
  if (g_frames) show(g_frames, Tone::plain, "%u", audio::frames_played());
}

}  // namespace

const Page kSound = {"sound", "Sound", build, refresh};

}  // namespace ui
