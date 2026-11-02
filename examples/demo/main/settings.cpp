#include "settings.h"

#include <cstdio>
#include <cstring>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace settings {
namespace {

constexpr const char* kTag = "settings";
constexpr const char* kNamespace = "demo";
constexpr const char* kKey = "values";

Values g_values;
SemaphoreHandle_t g_mutex = nullptr;

void use_default_network(Values& values) {
  std::snprintf(values.ssid, sizeof values.ssid, "%s", CONFIG_DEMO_WIFI_SSID);
  std::snprintf(values.password, sizeof values.password, "%s", CONFIG_DEMO_WIFI_PASSWORD);
}

void load() {
  nvs_handle_t handle;
  if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
  Values saved;
  size_t length = sizeof saved;
  // A blob of another size is from a build with a different struct, so it's left alone.
  if (nvs_get_blob(handle, kKey, &saved, &length) == ESP_OK && length == sizeof saved) g_values = saved;
  nvs_close(handle);
}

void save(const Values& values) {
  nvs_handle_t handle;
  esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &handle);
  if (err == ESP_OK) {
    err = nvs_set_blob(handle, kKey, &values, sizeof values);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
  }
  if (err != ESP_OK) ESP_LOGW(kTag, "couldn't save: %s", esp_err_to_name(err));
}

template <typename Change>
void update(Change change) {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  change(g_values);
  save(g_values);
  xSemaphoreGive(g_mutex);
}

}  // namespace

void start() {
  g_mutex = xSemaphoreCreateMutex();
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  use_default_network(g_values);
  load();
  update([](Values& values) { ++values.boots; });
  ESP_LOGI(kTag, "boot %lu, brightness %d, volume %d", static_cast<unsigned long>(g_values.boots),
           g_values.brightness, g_values.volume);
}

Values get() {
  xSemaphoreTake(g_mutex, portMAX_DELAY);
  const Values values = g_values;
  xSemaphoreGive(g_mutex);
  return values;
}

void set_brightness(int percent) {
  update([percent](Values& values) { values.brightness = static_cast<uint8_t>(percent); });
}

void set_volume(int percent) {
  update([percent](Values& values) { values.volume = static_cast<uint8_t>(percent); });
}

void set_wifi(const char* ssid, const char* password) {
  update([ssid, password](Values& values) {
    std::snprintf(values.ssid, sizeof values.ssid, "%s", ssid);
    std::snprintf(values.password, sizeof values.password, "%s", password);
  });
}

void reset() {
  update([](Values& values) {
    const uint32_t boots = values.boots;
    values = Values{};
    values.boots = boots;
    use_default_network(values);
  });
}

}  // namespace settings
