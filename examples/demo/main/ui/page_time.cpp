// Where the time comes from: the RTC at boot, SNTP once there's a network.
#include <cstdio>
#include <ctime>

#include "timekeeper.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace ui {
namespace {

lv_obj_t* g_utc = nullptr;
lv_obj_t* g_rtc = nullptr;
lv_obj_t* g_source = nullptr;
lv_obj_t* g_sync = nullptr;
lv_obj_t* g_clock = nullptr;

const char* source_name(timekeeper::Source source) {
  switch (source) {
    case timekeeper::Source::none:
      return "none";
    case timekeeper::Source::rtc:
      return "RTC";
    case timekeeper::Source::network:
      return "SNTP";
    case timekeeper::Source::console:
      return "console";
  }
  return "";
}

void build(lv_obj_t* body) {
  lv_obj_t* box = card(body);
  caption(box, "UTC");
  g_utc = lv_label_create(box);
  lv_obj_set_style_text_font(g_utc, narrow() ? &lv_font_montserrat_20 : &lv_font_montserrat_40, 0);
  lv_label_set_text(g_utc, "");

  lv_obj_t* details = card(body);
  g_rtc = row(details, "RTC chip");
  g_source = row(details, "Set by");
  g_sync = row(details, "SNTP");
  g_clock = row(details, "Clock");
}

void refresh() {
  if (timekeeper::known()) {
    const time_t utc = timekeeper::now();
    std::tm date;
    gmtime_r(&utc, &date);
    char text[12];
    std::strftime(text, sizeof text, "%H:%M:%S", &date);
    show(g_utc, Tone::plain, "%s", text);
  } else {
    show(g_utc, Tone::warning, "unknown");
  }
  const rtc::Status status = monitor::latest().rtc;
  show(g_rtc, status == rtc::Status::ok ? Tone::plain : Tone::warning, "%s", describe(status));
  show(g_source, Tone::plain, "%s", source_name(timekeeper::source()));
  const bool synced = network::status().time_synced;
  show(g_sync, Tone::plain, "%s", synced ? "synced" : "waiting");
  show(g_clock, Tone::plain, "%s", timekeeper::held() ? "held" : "running");
}

}  // namespace

const Page kTime = {"time", "Time", build, refresh};

}  // namespace ui
