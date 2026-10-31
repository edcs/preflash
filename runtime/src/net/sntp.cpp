#include <sys/time.h>

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "esp_log.h"
#include "esp_sntp.h"
#include "preflash/net.h"

namespace {

using std::chrono::milliseconds;

constexpr const char* kTag = "sntp";
constexpr int kServers = 3;
constexpr milliseconds kFirstSyncDelay{100};

struct Sntp {
  std::mutex mutex;
  std::condition_variable wake;
  bool running = false;
  uint64_t generation = 0;  // a new run or a stop retires the worker of the old one
  std::thread worker;
  sntp_sync_status_t status = SNTP_SYNC_STATUS_RESET;
  sntp_sync_time_cb_t callback = nullptr;
  sntp_sync_mode_t mode = SNTP_SYNC_MODE_IMMED;
  esp_sntp_operatingmode_t opmode = ESP_SNTP_OPMODE_POLL;
  uint32_t interval_ms = 3600000;  // the device's default of an hour
  std::string servers[kServers];
};

// Never destroyed: the worker may be asleep when the process ends.
Sntp& sntp() {
  static Sntp* s = new Sntp;
  return *s;
}

// Waits out each interval, then reports a sync if the network is up. The time itself
// is the Mac's, which is already right, so only the status and the callback change.
void work(uint64_t generation, milliseconds first) {
  Sntp& s = sntp();
  std::unique_lock<std::mutex> lock(s.mutex);
  milliseconds wait = first;
  for (;;) {
    s.wake.wait_for(lock, wait, [&] { return s.generation != generation; });
    if (s.generation != generation) return;
    if (!preflash::net_online()) {
      wait = milliseconds(50);  // keeps trying until a route appears
      continue;
    }
    s.status = SNTP_SYNC_STATUS_COMPLETED;
    sntp_sync_time_cb_t callback = s.callback;
    wait = milliseconds(s.interval_ms);
    lock.unlock();
    if (callback) {
      timeval now;
      gettimeofday(&now, nullptr);
      callback(&now);
    }
    lock.lock();
    if (s.generation != generation) return;
  }
}

// Lock held.
void retire_worker(Sntp& s, std::thread* out) {
  ++s.generation;
  s.running = false;
  *out = std::move(s.worker);
}

void join_worker(Sntp& s, std::thread& old) {
  s.wake.notify_all();
  if (!old.joinable()) return;
  // A sync callback may stop the service from the worker itself, which can't join itself.
  if (old.get_id() == std::this_thread::get_id()) old.detach();
  else old.join();
}

}  // namespace

extern "C" {

void esp_sntp_setoperatingmode(esp_sntp_operatingmode_t operating_mode) {
  std::lock_guard<std::mutex> lock(sntp().mutex);
  sntp().opmode = operating_mode;
}

void esp_sntp_setservername(uint8_t idx, const char* server) {
  if (idx >= kServers) return;
  std::lock_guard<std::mutex> lock(sntp().mutex);
  sntp().servers[idx] = server ? server : "";
}

const char* esp_sntp_getservername(uint8_t idx) {
  if (idx >= kServers) return nullptr;
  std::lock_guard<std::mutex> lock(sntp().mutex);
  const std::string& name = sntp().servers[idx];
  return name.empty() ? nullptr : name.c_str();
}

void esp_sntp_init(void) {
  Sntp& s = sntp();
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (s.running) return;
    s.status = SNTP_SYNC_STATUS_RESET;
    s.running = true;
    const uint64_t generation = ++s.generation;
    s.worker = std::thread(work, generation, kFirstSyncDelay);
  }
  ESP_LOGI(kTag, "using the Mac's clock: the system time is left alone");
}

void esp_sntp_stop(void) {
  Sntp& s = sntp();
  std::thread old;
  {
    std::lock_guard<std::mutex> lock(s.mutex);
    if (!s.running) return;
    retire_worker(s, &old);
  }
  join_worker(s, old);
}

bool esp_sntp_enabled(void) {
  std::lock_guard<std::mutex> lock(sntp().mutex);
  return sntp().running;
}

void sntp_set_time_sync_notification_cb(sntp_sync_time_cb_t callback) {
  std::lock_guard<std::mutex> lock(sntp().mutex);
  sntp().callback = callback;
}

void sntp_set_sync_mode(sntp_sync_mode_t sync_mode) {
  std::lock_guard<std::mutex> lock(sntp().mutex);
  sntp().mode = sync_mode;
}

sntp_sync_mode_t sntp_get_sync_mode(void) {
  std::lock_guard<std::mutex> lock(sntp().mutex);
  return sntp().mode;
}

sntp_sync_status_t sntp_get_sync_status(void) {
  std::lock_guard<std::mutex> lock(sntp().mutex);
  const sntp_sync_status_t status = sntp().status;
  if (status == SNTP_SYNC_STATUS_COMPLETED) sntp().status = SNTP_SYNC_STATUS_RESET;
  return status;
}

void sntp_set_sync_status(sntp_sync_status_t sync_status) {
  std::lock_guard<std::mutex> lock(sntp().mutex);
  sntp().status = sync_status;
}

void sntp_restart(void) {
  esp_sntp_stop();
  esp_sntp_init();
}

uint32_t sntp_get_sync_interval(void) {
  std::lock_guard<std::mutex> lock(sntp().mutex);
  return sntp().interval_ms;
}

void sntp_set_sync_interval(uint32_t interval_ms) {
  std::lock_guard<std::mutex> lock(sntp().mutex);
  sntp().interval_ms = interval_ms < 15000 ? 15000 : interval_ms;  // the device's floor
}

}  // extern "C"
