// Wi-Fi, then SNTP and an HTTPS request once there's an address. Every step can fail, and each
// failure is a state the screen shows. The app carries on with no network at all.
#pragma once

#include <cstddef>

#include "esp_err.h"

namespace network {

enum class Wifi { off, joining, joined, failed };
enum class Fetch { idle, running, ok, failed };

struct Status {
  Wifi wifi = Wifi::off;
  char ssid[33] = "";
  char address[16] = "";
  int signal_dbm = 0;
  int fail_reason = 0;  // wifi_err_reason_t of the last failure

  bool time_synced = false;

  Fetch fetch = Fetch::idle;
  int http_status = 0;
  int bytes = 0;
  esp_err_t fetch_error = ESP_OK;
};

// Starts joining the network saved in settings.
void start();
void join(const char* ssid, const char* password);
Status status();

// Fetches the check page, on the network's task.
void fetch();

// Runs work on the network's task, where it's free to block.
void run_later(void (*work)());

// Reads a page into out, cut off at capacity. Blocks: call it from run_later's work.
esp_err_t https_get(const char* url, char* out, size_t capacity, int* http_status, int* bytes);

}  // namespace network
