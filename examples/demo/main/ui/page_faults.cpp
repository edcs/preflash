// What the firmware sees of each part of the board. Nothing here knows why a part failed: a
// chip that stops answering looks the same whether it broke or someone forced it.
#include "board.h"
#include "esp_err.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace ui {
namespace {

lv_obj_t* g_rtc = nullptr;
lv_obj_t* g_motion = nullptr;
lv_obj_t* g_power = nullptr;
lv_obj_t* g_wifi = nullptr;
lv_obj_t* g_internet = nullptr;
lv_obj_t* g_detail = nullptr;

struct Findings {
  bool rtc_ok;
  bool motion_ok;
  bool power_ok;
  bool wifi_ok;
  bool internet_ok;
};

Findings look() {
  const monitor::Snapshot chips = monitor::latest();
  const network::Status net = network::status();
  Findings found = {};
  found.rtc_ok = chips.rtc == rtc::Status::ok;
  found.motion_ok = chips.motion.answered;
  found.power_ok = !board::has_power_chip() || chips.power.answered;
  found.wifi_ok = net.wifi != network::Wifi::failed;
  found.internet_ok = net.fetch != network::Fetch::failed;
  return found;
}

void show_finding(lv_obj_t* value, bool ok, const char* words) {
  show(value, ok ? Tone::good : Tone::bad, "%s", words);
}

void build(lv_obj_t* body) {
  lv_obj_t* box = card(body);
  caption(box, "What the firmware sees");
  g_rtc = row(box, "RTC");
  g_motion = row(box, "Motion");
  g_power = row(box, "Power");
  g_wifi = row(box, "Wi-Fi");
  g_internet = row(box, "Internet");
  g_detail = caption(body, " ");
  spacer(body);
  caption(body, "Each row is what the part gave back when asked. The firmware can't tell why.");
}

void refresh() {
  const monitor::Snapshot chips = monitor::latest();
  const network::Status net = network::status();
  const Findings found = look();
  show_finding(g_rtc, found.rtc_ok, describe(chips.rtc));
  show_finding(g_motion, found.motion_ok, found.motion_ok ? "ok" : "no answer");
  if (board::has_power_chip()) {
    show_finding(g_power, found.power_ok, found.power_ok ? "ok" : "no answer");
  } else {
    show(g_power, Tone::plain, "no chip");
  }
  show_finding(g_wifi, found.wifi_ok, describe(net.wifi));
  show_finding(g_internet, found.internet_ok, found.internet_ok ? describe(net.fetch) : "failed");
  if (net.wifi == network::Wifi::failed) {
    show(g_detail, Tone::warning, "Wi-Fi gave up, reason %d", net.fail_reason);
  } else if (net.fetch == network::Fetch::failed) {
    show(g_detail, Tone::warning, "%s", esp_err_to_name(net.fetch_error));
  } else {
    show(g_detail, Tone::plain, " ");
  }
}

}  // namespace

int faults_showing() {
  const Findings found = look();
  return !found.rtc_ok + !found.motion_ok + !found.power_ok + !found.wifi_ok + !found.internet_ok;
}

const Page kFaults = {"faults", "Faults", build, refresh};

}  // namespace ui
