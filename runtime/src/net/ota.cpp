#include <atomic>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_https_ota.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"

namespace {

constexpr const char* kTag = "ota";

// Two app slots, laid out as a project's partition table usually has them.
const esp_partition_t kSlot0 = {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0,
                                0x10000, 0x300000, 0x1000, "ota_0", false, false};
const esp_partition_t kSlot1 = {ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1,
                                0x310000, 0x300000, 0x1000, "ota_1", false, false};

// Says why an update can't happen, once, and only when firmware actually tries one:
// a project that never reaches here never sees the message.
void note_limitation() {
  static std::atomic<bool> said{false};
  if (said.exchange(true)) return;
  ESP_LOGW(kTag,
           "firmware updates are not simulated: the check finds nothing to install and "
           "a download or a slot change fails");
}

}  // namespace

extern "C" {

const esp_partition_t* esp_ota_get_running_partition(void) { return &kSlot0; }
const esp_partition_t* esp_ota_get_boot_partition(void) { return &kSlot0; }

const esp_partition_t* esp_ota_get_next_update_partition(const esp_partition_t* start_from) {
  // Searching on from a slot lands on the other one; from nowhere, on the one after the running app.
  if (!start_from || start_from == &kSlot0) return &kSlot1;
  if (start_from == &kSlot1) return &kSlot0;
  return nullptr;
}

const esp_partition_t* esp_ota_get_last_invalid_partition(void) { return nullptr; }

esp_err_t esp_ota_get_state_partition(const esp_partition_t* partition,
                                      esp_ota_img_states_t* ota_state) {
  if (!partition || !ota_state) return ESP_ERR_INVALID_ARG;
  if (partition != &kSlot0 && partition != &kSlot1) return ESP_ERR_NOT_SUPPORTED;
  // The running app is always settled, and the spare slot holds no image.
  if (partition == &kSlot1) return ESP_ERR_NOT_FOUND;
  *ota_state = ESP_OTA_IMG_VALID;
  return ESP_OK;
}

esp_err_t esp_ota_get_partition_description(const esp_partition_t* partition,
                                            esp_app_desc_t* app_desc) {
  if (!partition || !app_desc) return ESP_ERR_INVALID_ARG;
  if (partition == &kSlot0) {
    *app_desc = *esp_app_get_description();
    return ESP_OK;
  }
  // An empty slot has no app description to find, so nothing newer is waiting in it.
  return ESP_ERR_NOT_FOUND;
}

esp_err_t esp_ota_set_boot_partition(const esp_partition_t* partition) {
  if (!partition) return ESP_ERR_INVALID_ARG;
  if (partition == &kSlot0) return ESP_OK;  // already the boot slot
  note_limitation();
  return ESP_ERR_OTA_VALIDATE_FAILED;
}

esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }

esp_err_t esp_image_verify(esp_image_load_mode_t, const esp_partition_pos_t*,
                           esp_image_metadata_t*) {
  return ESP_ERR_IMAGE_INVALID;
}

esp_err_t esp_https_ota(const esp_https_ota_config_t*) {
  note_limitation();
  return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t esp_https_ota_begin(const esp_https_ota_config_t* config, esp_https_ota_handle_t* handle) {
  if (!config || !handle) return ESP_ERR_INVALID_ARG;
  note_limitation();
  *handle = nullptr;
  return ESP_ERR_NOT_SUPPORTED;
}

// A handle is never handed out, so the calls that take one can only come from a
// caller that skipped the failed begin. Tell it so rather than carry on.
esp_err_t esp_https_ota_perform(esp_https_ota_handle_t) { return ESP_ERR_INVALID_ARG; }
bool esp_https_ota_is_complete_data_received(esp_https_ota_handle_t) { return false; }
esp_err_t esp_https_ota_finish(esp_https_ota_handle_t) { return ESP_ERR_INVALID_ARG; }
esp_err_t esp_https_ota_abort(esp_https_ota_handle_t) { return ESP_ERR_INVALID_ARG; }
esp_err_t esp_https_ota_get_img_desc(esp_https_ota_handle_t, esp_app_desc_t*) {
  return ESP_ERR_INVALID_ARG;
}
int esp_https_ota_get_image_len_read(esp_https_ota_handle_t) { return -1; }

}  // extern "C"
