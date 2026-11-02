#include "network.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <sys/time.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "settings.h"
#include "timekeeper.h"

namespace network {
namespace {

constexpr const char* kTag = "network";
constexpr int kJoinAttempts = 3;
constexpr int kHttpTimeoutMs = 8000;
constexpr const char* kTimeServer = "pool.ntp.org";
constexpr int kQueueLength = 8;

std::mutex g_mutex;
Status g_status;
int g_attempts = 0;
QueueHandle_t g_work = nullptr;

template <typename Change>
void change_status(Change change) {
  std::lock_guard lock(g_mutex);
  change(g_status);
}

void on_time_synced(timeval* tv) {
  ESP_LOGI(kTag, "time synced");
  timekeeper::set(tv->tv_sec, timekeeper::Source::network);
  change_status([](Status& s) { s.time_synced = true; });
}

void start_time_sync() {
  if (esp_sntp_enabled()) return;
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, kTimeServer);
  sntp_set_time_sync_notification_cb(on_time_synced);
  esp_sntp_init();
}

void on_wifi_event(void*, esp_event_base_t, int32_t id, void* data) {
  if (id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (id == WIFI_EVENT_STA_CONNECTED) {
    wifi_ap_record_t access_point = {};
    const bool known = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
    change_status([&](Status& s) { s.signal_dbm = known ? access_point.rssi : 0; });
  } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
    const auto* event = static_cast<const wifi_event_sta_disconnected_t*>(data);
    if (++g_attempts < kJoinAttempts) {
      esp_wifi_connect();
      return;
    }
    ESP_LOGW(kTag, "gave up joining: reason %d", event->reason);
    change_status([event](Status& s) {
      s.wifi = Wifi::failed;
      s.fail_reason = event->reason;
      s.address[0] = '\0';
    });
  }
}

void on_got_address(void*, esp_event_base_t, int32_t, void* data) {
  const auto* event = static_cast<const ip_event_got_ip_t*>(data);
  g_attempts = 0;
  change_status([event](Status& s) {
    s.wifi = Wifi::joined;
    std::snprintf(s.address, sizeof s.address, IPSTR, IP2STR(&event->ip_info.ip));
  });
  ESP_LOGI(kTag, "joined");
  start_time_sync();
  fetch();
}

void connect_to(const char* ssid, const char* password) {
  wifi_config_t config = {};
  std::snprintf(reinterpret_cast<char*>(config.sta.ssid), sizeof config.sta.ssid, "%s", ssid);
  std::snprintf(reinterpret_cast<char*>(config.sta.password), sizeof config.sta.password, "%s", password);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &config));
  g_attempts = 0;
  change_status([ssid](Status& s) {
    s.wifi = Wifi::joining;
    std::snprintf(s.ssid, sizeof s.ssid, "%s", ssid);
  });
}

void run_fetch() {
  change_status([](Status& s) {
    s.fetch = Fetch::running;
    s.fetch_error = ESP_OK;
  });
  int http_status = 0;
  int bytes = 0;
  char start_of_page[64];
  const esp_err_t err = https_get(CONFIG_DEMO_HTTPS_URL, start_of_page, sizeof start_of_page, &http_status, &bytes);
  ESP_LOGI(kTag, "fetch: %s, status %d, %d bytes", esp_err_to_name(err), http_status, bytes);
  change_status([=](Status& s) {
    s.fetch = err == ESP_OK && http_status == 200 ? Fetch::ok : Fetch::failed;
    s.fetch_error = err;
    s.http_status = http_status;
    s.bytes = bytes;
  });
}

void work_task(void*) {
  void (*work)() = nullptr;
  for (;;) {
    if (xQueueReceive(g_work, &work, portMAX_DELAY) == pdTRUE) work();
  }
}

struct Page {
  char* out;
  size_t capacity;
  int length;
};

esp_err_t on_http_event(esp_http_client_event_t* event) {
  if (event->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
  auto* page = static_cast<Page*>(event->user_data);
  const size_t used = page->length;
  if (used + 1 < page->capacity) {
    const size_t room = page->capacity - 1 - used;
    const size_t take = std::min(room, static_cast<size_t>(event->data_len));
    std::memcpy(page->out + used, event->data, take);
    page->out[used + take] = '\0';
  }
  page->length += event->data_len;
  return ESP_OK;
}

}  // namespace

void start() {
  g_work = xQueueCreate(kQueueLength, sizeof(void (*)()));
  ESP_ERROR_CHECK(g_work ? ESP_OK : ESP_ERR_NO_MEM);
  ESP_ERROR_CHECK(xTaskCreate(work_task, "network", 8192, nullptr, 3, nullptr) == pdPASS ? ESP_OK : ESP_FAIL);

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_address, nullptr));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

  const settings::Values saved = settings::get();
  connect_to(saved.ssid, saved.password);
  ESP_ERROR_CHECK(esp_wifi_start());
}

void join(const char* ssid, const char* password) {
  esp_wifi_disconnect();
  connect_to(ssid, password);
  esp_wifi_connect();
}

Status status() {
  std::lock_guard lock(g_mutex);
  return g_status;
}

void fetch() { run_later(run_fetch); }

void run_later(void (*work)()) {
  if (xQueueSend(g_work, &work, 0) != pdTRUE) ESP_LOGW(kTag, "too much waiting: dropped a job");
}

esp_err_t https_get(const char* url, char* out, size_t capacity, int* http_status, int* bytes) {
  Page page = {out, capacity, 0};
  out[0] = '\0';
  esp_http_client_config_t config = {};
  config.url = url;
  config.timeout_ms = kHttpTimeoutMs;
  config.event_handler = on_http_event;
  config.user_data = &page;
  config.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return ESP_ERR_NO_MEM;
  const esp_err_t err = esp_http_client_perform(client);
  *http_status = esp_http_client_get_status_code(client);
  *bytes = page.length;
  esp_http_client_cleanup(client);
  return err;
}

}  // namespace network
