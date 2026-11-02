// Wi-Fi, then an HTTPS request over it. Both can fail, and the page says how.
#include "esp_err.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace ui {
namespace {

lv_obj_t* g_wifi = nullptr;
lv_obj_t* g_ssid = nullptr;
lv_obj_t* g_address = nullptr;
lv_obj_t* g_signal = nullptr;
lv_obj_t* g_fetch = nullptr;
lv_obj_t* g_detail = nullptr;

void on_fetch(lv_event_t*) { network::fetch(); }

void build(lv_obj_t* body) {
  lv_obj_t* wifi = card(body);
  g_wifi = row(wifi, "Wi-Fi");
  g_ssid = row(wifi, "Network");
  g_address = row(wifi, "Address");
  g_signal = row(wifi, "Signal");

  lv_obj_t* https = card(body);
  g_fetch = row(https, "HTTPS");
  g_detail = caption(https, "");

  spacer(body);
  button(body, LV_SYMBOL_REFRESH "  Fetch again", on_fetch);
}

void refresh() {
  const network::Status net = network::status();
  const bool joined = net.wifi == network::Wifi::joined;
  show(g_wifi, net.wifi == network::Wifi::failed ? Tone::warning : Tone::plain, "%s", describe(net.wifi));
  show(g_ssid, Tone::plain, "%s", net.ssid);
  show(g_address, Tone::plain, "%s", joined ? net.address : "none");
  if (joined) {
    show(g_signal, Tone::plain, "%d dBm", net.signal_dbm);
  } else {
    show(g_signal, Tone::plain, "none");
  }

  show(g_fetch, net.fetch == network::Fetch::failed ? Tone::warning : Tone::plain, "%s", describe(net.fetch));
  if (net.fetch == network::Fetch::ok) {
    show(g_detail, Tone::plain, "%d, %d bytes", net.http_status, net.bytes);
  } else if (net.fetch == network::Fetch::failed) {
    show(g_detail, Tone::warning, "%s", esp_err_to_name(net.fetch_error));
  } else {
    show(g_detail, Tone::plain, "no answer yet");
  }
}

}  // namespace

const Page kNetwork = {"network", "Network", build, refresh};

}  // namespace ui
