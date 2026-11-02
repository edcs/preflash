// Which app slot is running, and whether a newer version exists.
#include "esp_err.h"
#include "ui/ui.h"
#include "ui/widgets.h"

namespace ui {
namespace {

lv_obj_t* g_version = nullptr;
lv_obj_t* g_running = nullptr;
lv_obj_t* g_spare = nullptr;
lv_obj_t* g_confirmed = nullptr;
lv_obj_t* g_check = nullptr;
lv_obj_t* g_detail = nullptr;

void on_check(lv_event_t*) { update::check(); }

void build(lv_obj_t* body) {
  lv_obj_t* slots = card(body);
  g_version = row(slots, "Version");
  g_running = row(slots, "Running slot");
  g_spare = row(slots, "Spare slot");
  g_confirmed = row(slots, "Confirmed");

  lv_obj_t* check = card(body);
  g_check = row(check, "Newest");
  g_detail = caption(check, "");

  spacer(body);
  button(body, LV_SYMBOL_REFRESH "  Check now", on_check);
}

void refresh() {
  const update::Status status = update::status();
  show(g_version, Tone::plain, "%s", status.version);
  show(g_running, Tone::plain, "%s", status.running_slot);
  show(g_spare, Tone::plain, "%s", status.spare_slot);
  show(g_confirmed, Tone::plain, "%s", status.confirmed ? "yes" : "no");
  show(g_check, status.check == update::Check::failed ? Tone::warning : Tone::plain, "%s", describe(status.check));
  if (status.check == update::Check::newer) {
    show(g_detail, Tone::plain, "%s is out", status.newest);
  } else if (status.check == update::Check::failed) {
    show(g_detail, Tone::warning, "%s", esp_err_to_name(status.error));
  } else {
    show(g_detail, Tone::plain, " ");
  }
}

}  // namespace

const Page kUpdate = {"update", "Update", build, refresh};

}  // namespace ui
