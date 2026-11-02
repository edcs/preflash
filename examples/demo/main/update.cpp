#include "update.h"

#include <cstdio>
#include <cstring>
#include <mutex>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "network.h"
#include "sdkconfig.h"

namespace update {
namespace {

constexpr const char* kTag = "update";

std::mutex g_mutex;
Status g_status;

void finish(Check result, const char* newest, esp_err_t error) {
  std::lock_guard lock(g_mutex);
  g_status.check = result;
  std::snprintf(g_status.newest, sizeof g_status.newest, "%s", newest);
  g_status.error = error;
}

void run_check() {
  {
    std::lock_guard lock(g_mutex);
    g_status.check = Check::running;
  }
  char body[32];
  int http_status = 0;
  int bytes = 0;
  const esp_err_t err = network::https_get(CONFIG_DEMO_UPDATE_URL, body, sizeof body, &http_status, &bytes);
  if (err != ESP_OK || http_status != 200) {
    ESP_LOGW(kTag, "check failed: %s, status %d", esp_err_to_name(err), http_status);
    finish(Check::failed, "", err);
    return;
  }
  body[std::strcspn(body, "\r\n")] = '\0';
  finish(std::strcmp(body, esp_app_get_description()->version) == 0 ? Check::current : Check::newer, body, ESP_OK);
}

}  // namespace

void start() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
  esp_ota_get_state_partition(running, &state);
  if (state == ESP_OTA_IMG_PENDING_VERIFY) esp_ota_mark_app_valid_cancel_rollback();

  esp_ota_get_state_partition(running, &state);
  const esp_partition_t* spare = esp_ota_get_next_update_partition(nullptr);
  std::lock_guard lock(g_mutex);
  std::snprintf(g_status.version, sizeof g_status.version, "%s", esp_app_get_description()->version);
  std::snprintf(g_status.running_slot, sizeof g_status.running_slot, "%s", running->label);
  std::snprintf(g_status.spare_slot, sizeof g_status.spare_slot, "%s", spare ? spare->label : "none");
  g_status.confirmed = state == ESP_OTA_IMG_VALID;
}

Status status() {
  std::lock_guard lock(g_mutex);
  return g_status;
}

void check() { network::run_later(run_check); }

}  // namespace update
