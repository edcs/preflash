#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "preflash/fail.h"
#include "preflash/net.h"

ESP_EVENT_DEFINE_BASE(WIFI_EVENT);
ESP_EVENT_DEFINE_BASE(IP_EVENT);

// Defined with the netif it fills in, and posted from the radio's timer.
void preflash_net_post_got_ip();

namespace {

using Clock = std::chrono::steady_clock;
using std::chrono::milliseconds;

constexpr const char* kTag = "wifi";

// Long enough for a caller to see "connecting" before it ends, short enough to keep
// a run lively.
constexpr milliseconds kJoinDelay{150};
constexpr milliseconds kIpDelay{100};
constexpr milliseconds kScanDelay{300};

std::atomic<bool> g_force_failure{false};
std::atomic<bool> g_online{true};

struct FakeNetwork {
  const char* ssid;
  int8_t rssi;
  wifi_auth_mode_t auth;
  uint8_t channel;
};

// Obviously invented names, so no one mistakes them for a real neighbourhood.
const FakeNetwork kNetworks[] = {
    {"Preflash Fake Home", -42, WIFI_AUTH_WPA2_PSK, 1},
    {"Preflash Fake Guest", -58, WIFI_AUTH_OPEN, 6},
    {"Preflash Fake Neighbour", -67, WIFI_AUTH_WPA2_PSK, 11},
    {"Preflash Fake Cafe", -74, WIFI_AUTH_WPA2_WPA3_PSK, 6},
    {"Preflash Fake Hotspot", -81, WIFI_AUTH_WPA2_PSK, 3},
};

void fake_bssid(size_t index, uint8_t out[6]) {
  const uint8_t base[6] = {0x02, 0x50, 0x46, 0x4c, 0x00, 0x00};  // locally administered
  std::memcpy(out, base, 6);
  out[5] = static_cast<uint8_t>(index + 1);
}

// Timed work for the radio, run in order on one thread. Work queued before a stop or
// a new join is dropped by checking the epoch it was queued under.
class Scheduler {
 public:
  void start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thread_.joinable()) return;
    quit_ = false;
    thread_ = std::thread([this] { run(); });
  }
  void stop() {
    std::thread joining;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      quit_ = true;
      timers_.clear();
      joining = std::move(thread_);
    }
    wake_.notify_all();
    if (joining.joinable()) {
      if (joining.get_id() == std::this_thread::get_id()) joining.detach();
      else joining.join();
    }
  }
  void after(milliseconds delay, std::function<void()> work) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      timers_.emplace(Clock::now() + delay, std::move(work));
    }
    wake_.notify_all();
  }

 private:
  void run() {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!quit_) {
      if (timers_.empty()) {
        wake_.wait(lock);
        continue;
      }
      const auto due = timers_.begin()->first;
      if (Clock::now() < due) {
        wake_.wait_until(lock, due);
        continue;
      }
      auto work = std::move(timers_.begin()->second);
      timers_.erase(timers_.begin());
      lock.unlock();
      work();
      lock.lock();
    }
  }

  std::mutex mutex_;
  std::condition_variable wake_;
  std::multimap<Clock::time_point, std::function<void()>> timers_;
  std::thread thread_;
  bool quit_ = false;
};

enum class Link { Down, Joining, Up };

struct Radio {
  std::mutex mutex;
  bool inited = false;
  bool started = false;
  wifi_mode_t mode = WIFI_MODE_NULL;
  wifi_config_t config{};
  bool config_set = false;
  Link link = Link::Down;
  bool scanning = false;
  std::vector<wifi_ap_record_t> results;
  bool have_results = false;
  uint64_t epoch = 0;     // bumped when pending join work should lapse
  uint64_t scan_gen = 0;  // bumped when a pending scan result should lapse
  uint8_t scan_id = 0;
  wifi_ap_record_t ap{};  // the network the link is up on
  Scheduler scheduler;
};

// Never destroyed: its thread may still be sleeping when the process ends.
Radio& radio() {
  static Radio* r = new Radio;
  return *r;
}

void post(esp_event_base_t base, int32_t id, const void* data, size_t size) {
  // No default loop means no one is listening, which the device allows too.
  esp_event_post(base, id, data, size, pdMS_TO_TICKS(100));
}

void post_disconnected(const wifi_ap_record_t& ap, uint8_t reason) {
  wifi_event_sta_disconnected_t event{};
  std::memcpy(event.ssid, ap.ssid, 32);
  event.ssid_len = static_cast<uint8_t>(strnlen(reinterpret_cast<const char*>(ap.ssid), 32));
  std::memcpy(event.bssid, ap.bssid, 6);
  event.reason = reason;
  event.rssi = ap.rssi;
  post(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &event, sizeof event);
}

wifi_ap_record_t record_for(const FakeNetwork& n, size_t index) {
  wifi_ap_record_t r{};
  fake_bssid(index, r.bssid);
  std::strncpy(reinterpret_cast<char*>(r.ssid), n.ssid, sizeof r.ssid - 1);
  r.primary = n.channel;
  r.second = WIFI_SECOND_CHAN_NONE;
  r.rssi = n.rssi;
  r.authmode = n.auth;
  r.pairwise_cipher = n.auth == WIFI_AUTH_OPEN ? WIFI_CIPHER_TYPE_NONE : WIFI_CIPHER_TYPE_CCMP;
  r.group_cipher = r.pairwise_cipher;
  return r;
}

// The network a join lands on. The simulator has no real air to search, so any name
// the firmware asks for is found; a known fake keeps its own signal and security.
wifi_ap_record_t ap_for(const std::string& ssid) {
  for (size_t i = 0; i < sizeof kNetworks / sizeof kNetworks[0]; ++i) {
    if (ssid == kNetworks[i].ssid) return record_for(kNetworks[i], i);
  }
  const FakeNetwork any{"", -50, WIFI_AUTH_WPA2_PSK, 6};
  wifi_ap_record_t r = record_for(any, 7);
  std::strncpy(reinterpret_cast<char*>(r.ssid), ssid.c_str(), sizeof r.ssid - 1);
  return r;
}

// Ends the join under way: the step that decides how it goes.
void finish_join(uint64_t epoch) {
  Radio& r = radio();
  wifi_ap_record_t ap;
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.epoch != epoch || r.link != Link::Joining) return;
    ap = ap_for(reinterpret_cast<const char*>(r.config.sta.ssid));
    if (g_force_failure.load()) {
      r.link = Link::Down;
    } else {
      r.link = Link::Up;
      r.ap = ap;
    }
  }
  if (g_force_failure.load()) {
    post_disconnected(ap, WIFI_REASON_AUTH_FAIL);
    return;
  }
  wifi_event_sta_connected_t connected{};
  std::memcpy(connected.ssid, ap.ssid, 32);
  connected.ssid_len = static_cast<uint8_t>(strnlen(reinterpret_cast<const char*>(ap.ssid), 32));
  std::memcpy(connected.bssid, ap.bssid, 6);
  connected.channel = ap.primary;
  connected.authmode = ap.authmode;
  connected.aid = 1;
  post(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED, &connected, sizeof connected);
  r.scheduler.after(kIpDelay, [epoch] {
    Radio& radio_ = radio();
    {
      std::lock_guard<std::mutex> lock(radio_.mutex);
      if (radio_.epoch != epoch || radio_.link != Link::Up) return;
    }
    preflash_net_post_got_ip();
  });
}

void finish_scan(uint64_t gen, std::string only_ssid) {
  Radio& r = radio();
  wifi_event_sta_scan_done_t done{};
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    if (r.scan_gen != gen || !r.scanning) return;
    r.scanning = false;
    r.results.clear();
    for (size_t i = 0; i < sizeof kNetworks / sizeof kNetworks[0]; ++i) {
      if (!only_ssid.empty() && only_ssid != kNetworks[i].ssid) continue;
      r.results.push_back(record_for(kNetworks[i], i));
    }
    r.have_results = true;
    done.status = 0;
    done.number = static_cast<uint8_t>(r.results.size());
    done.scan_id = ++r.scan_id;
  }
  post(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &done, sizeof done);
}

// Lock held. Everything pending lapses, and a join or scan in flight is over.
void cancel_pending(Radio& r) {
  ++r.epoch;
  ++r.scan_gen;
  r.scanning = false;
}

struct NetifState {
  std::mutex mutex;
  bool inited = false;
  bool has_default_sta = false;
  std::string hostname = "espressif";
  esp_netif_ip_info_t ip{};
};

NetifState& netif_state() {
  static NetifState* s = new NetifState;
  return *s;
}

}  // namespace

struct esp_netif_obj {
  int unused;
};

void preflash_net_post_got_ip() {
  esp_netif_obj* netif;
  ip_event_got_ip_t event{};
  {
    NetifState& n = netif_state();
    std::lock_guard<std::mutex> lock(n.mutex);
    static esp_netif_obj the_netif;
    netif = &the_netif;
    n.ip.ip.addr = 0x0200000a;       // 10.0.0.2, network byte order
    n.ip.netmask.addr = 0x00ffffff;  // 255.255.255.0
    n.ip.gw.addr = 0x0100000a;       // 10.0.0.1
    event.esp_netif = netif;
    event.ip_info = n.ip;
    event.ip_changed = true;
  }
  esp_event_post(IP_EVENT, IP_EVENT_STA_GOT_IP, &event, sizeof event, pdMS_TO_TICKS(100));
}

namespace preflash {

void wifi_force_join_failure(bool on) { g_force_failure = on; }
void net_set_online(bool online) { g_online = online; }
bool net_online() { return g_online; }

}  // namespace preflash

extern "C" {

esp_err_t esp_netif_init(void) {
  NetifState& n = netif_state();
  std::lock_guard<std::mutex> lock(n.mutex);
  n.inited = true;
  return ESP_OK;
}

esp_err_t esp_netif_deinit(void) {
  NetifState& n = netif_state();
  std::lock_guard<std::mutex> lock(n.mutex);
  n.inited = false;
  return ESP_OK;
}

esp_netif_t* esp_netif_create_default_wifi_sta(void) {
  NetifState& n = netif_state();
  std::lock_guard<std::mutex> lock(n.mutex);
  if (!n.inited || n.has_default_sta) {
    // The device asserts here, and a retry loop that makes a second one hides it.
    preflash_unsupported("esp_netif_create_default_wifi_sta",
                         n.inited ? "a default station interface already exists"
                                  : "esp_netif_init was not called first");
  }
  n.has_default_sta = true;
  static esp_netif_obj the_netif;
  return &the_netif;
}

void esp_netif_destroy(esp_netif_t*) {
  NetifState& n = netif_state();
  std::lock_guard<std::mutex> lock(n.mutex);
  n.has_default_sta = false;
  n.ip = {};
}

void esp_netif_destroy_default_wifi(void* netif) { esp_netif_destroy(static_cast<esp_netif_t*>(netif)); }

esp_err_t esp_netif_get_ip_info(esp_netif_t* netif, esp_netif_ip_info_t* ip_info) {
  if (!netif || !ip_info) return ESP_ERR_INVALID_ARG;
  NetifState& n = netif_state();
  std::lock_guard<std::mutex> lock(n.mutex);
  *ip_info = n.ip;
  return ESP_OK;
}

esp_err_t esp_netif_set_hostname(esp_netif_t* netif, const char* hostname) {
  if (!netif || !hostname) return ESP_ERR_INVALID_ARG;
  NetifState& n = netif_state();
  std::lock_guard<std::mutex> lock(n.mutex);
  n.hostname = hostname;
  return ESP_OK;
}

esp_err_t esp_netif_get_hostname(esp_netif_t* netif, const char** hostname) {
  if (!netif || !hostname) return ESP_ERR_INVALID_ARG;
  NetifState& n = netif_state();
  std::lock_guard<std::mutex> lock(n.mutex);
  *hostname = n.hostname.c_str();
  return ESP_OK;
}

esp_err_t esp_wifi_init(const wifi_init_config_t* config) {
  if (!config) return ESP_ERR_INVALID_ARG;
  if (config->magic != WIFI_INIT_CONFIG_MAGIC) return ESP_ERR_NOT_SUPPORTED;
  Radio& r = radio();
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    r.inited = true;
  }
  r.scheduler.start();
  return ESP_OK;
}

esp_err_t esp_wifi_deinit(void) {
  Radio& r = radio();
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
    if (r.started) return ESP_ERR_WIFI_NOT_STOPPED;
    r.inited = false;
    r.mode = WIFI_MODE_NULL;
    r.config_set = false;
    r.have_results = false;
    r.results.clear();
    cancel_pending(r);
  }
  r.scheduler.stop();
  return ESP_OK;
}

esp_err_t esp_wifi_set_storage(wifi_storage_t) {
  // Nothing is stored either way: the firmware keeps its own credentials.
  return radio().inited ? ESP_OK : ESP_ERR_WIFI_NOT_INIT;
}

esp_err_t esp_wifi_set_mode(wifi_mode_t mode) {
  if (mode == WIFI_MODE_AP || mode == WIFI_MODE_APSTA || mode == WIFI_MODE_NAN) {
    preflash_unsupported("esp_wifi_set_mode", "only the station mode is simulated");
  }
  if (mode >= WIFI_MODE_MAX) return ESP_ERR_INVALID_ARG;
  Radio& r = radio();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
  r.mode = mode;
  return ESP_OK;
}

esp_err_t esp_wifi_get_mode(wifi_mode_t* mode) {
  if (!mode) return ESP_ERR_INVALID_ARG;
  Radio& r = radio();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
  *mode = r.mode;
  return ESP_OK;
}

esp_err_t esp_wifi_start(void) {
  Radio& r = radio();
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
    if (r.mode == WIFI_MODE_NULL) return ESP_ERR_WIFI_MODE;
    if (r.started) return ESP_OK;
    r.started = true;
  }
  post(WIFI_EVENT, WIFI_EVENT_STA_START, nullptr, 0);
  return ESP_OK;
}

esp_err_t esp_wifi_stop(void) {
  Radio& r = radio();
  bool was_up;
  wifi_ap_record_t ap;
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
    if (!r.started) return ESP_ERR_WIFI_NOT_STARTED;
    r.started = false;
    was_up = r.link != Link::Down;
    ap = r.link == Link::Up ? r.ap : ap_for(reinterpret_cast<const char*>(r.config.sta.ssid));
    r.link = Link::Down;
    r.have_results = false;
    r.results.clear();
    cancel_pending(r);
  }
  if (was_up) post_disconnected(ap, WIFI_REASON_ASSOC_LEAVE);
  post(WIFI_EVENT, WIFI_EVENT_STA_STOP, nullptr, 0);
  return ESP_OK;
}

esp_err_t esp_wifi_set_config(wifi_interface_t interface, wifi_config_t* conf) {
  if (interface != WIFI_IF_STA) {
    preflash_unsupported("esp_wifi_set_config", "only the station interface is simulated");
  }
  if (!conf) return ESP_ERR_INVALID_ARG;
  Radio& r = radio();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
  r.config = *conf;
  r.config_set = true;
  return ESP_OK;
}

esp_err_t esp_wifi_get_config(wifi_interface_t interface, wifi_config_t* conf) {
  if (interface != WIFI_IF_STA) {
    preflash_unsupported("esp_wifi_get_config", "only the station interface is simulated");
  }
  if (!conf) return ESP_ERR_INVALID_ARG;
  Radio& r = radio();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
  *conf = r.config;
  return ESP_OK;
}

esp_err_t esp_wifi_connect(void) {
  Radio& r = radio();
  uint64_t epoch;
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
    if (!r.started) return ESP_ERR_WIFI_NOT_STARTED;
    if (r.scanning) return ESP_ERR_WIFI_STATE;  // the device refuses while a scan runs
    if (r.link == Link::Up) return ESP_ERR_WIFI_CONN;
    if (!r.config_set || r.config.sta.ssid[0] == 0) return ESP_ERR_WIFI_SSID;
    r.link = Link::Joining;
    epoch = ++r.epoch;  // a join already under way is replaced
  }
  r.scheduler.after(kJoinDelay, [epoch] { finish_join(epoch); });
  return ESP_OK;
}

esp_err_t esp_wifi_disconnect(void) {
  Radio& r = radio();
  wifi_ap_record_t ap;
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
    if (!r.started) return ESP_ERR_WIFI_NOT_STARTED;
    if (r.link == Link::Down) return ESP_ERR_WIFI_NOT_CONNECT;
    ap = r.link == Link::Up ? r.ap : ap_for(reinterpret_cast<const char*>(r.config.sta.ssid));
    r.link = Link::Down;
    ++r.epoch;
  }
  // A leave the firmware asked for still reports, with the reason the device gives.
  post_disconnected(ap, WIFI_REASON_ASSOC_LEAVE);
  return ESP_OK;
}

esp_err_t esp_wifi_set_ps(wifi_ps_type_t) { return radio().inited ? ESP_OK : ESP_ERR_WIFI_NOT_INIT; }

esp_err_t esp_wifi_scan_start(const wifi_scan_config_t* config, bool block) {
  Radio& r = radio();
  uint64_t epoch;
  std::string only;
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
    if (!r.started) return ESP_ERR_WIFI_NOT_STARTED;
    if (r.link == Link::Joining) return ESP_ERR_WIFI_STATE;
    if (config && config->ssid) only = reinterpret_cast<const char*>(config->ssid);
    r.scanning = true;
    r.have_results = false;
    epoch = ++r.scan_gen;  // a scan already running is replaced
  }
  if (block) {
    std::this_thread::sleep_for(kScanDelay);
    finish_scan(epoch, only);
    return ESP_OK;
  }
  r.scheduler.after(kScanDelay, [epoch, only] { finish_scan(epoch, only); });
  return ESP_OK;
}

esp_err_t esp_wifi_scan_stop(void) {
  Radio& r = radio();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
  if (!r.started) return ESP_ERR_WIFI_NOT_STARTED;
  if (r.link == Link::Joining) return ESP_ERR_WIFI_STATE;
  r.scanning = false;
  ++r.scan_gen;  // the done event no longer follows
  return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_num(uint16_t* number) {
  if (!number) return ESP_ERR_INVALID_ARG;
  Radio& r = radio();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
  if (!r.started) return ESP_ERR_WIFI_NOT_STARTED;
  *number = r.have_results ? static_cast<uint16_t>(r.results.size()) : 0;
  return ESP_OK;
}

esp_err_t esp_wifi_scan_get_ap_records(uint16_t* number, wifi_ap_record_t* ap_records) {
  if (!number || !ap_records) return ESP_ERR_INVALID_ARG;
  Radio& r = radio();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
  if (!r.started) return ESP_ERR_WIFI_NOT_STARTED;
  if (!r.have_results) return ESP_FAIL;  // no scan has finished
  const size_t n = std::min<size_t>(*number, r.results.size());
  std::copy_n(r.results.begin(), n, ap_records);
  *number = static_cast<uint16_t>(n);
  r.have_results = false;
  r.results.clear();
  return ESP_OK;
}

esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t* ap_info) {
  if (!ap_info) return ESP_ERR_INVALID_ARG;
  Radio& r = radio();
  std::lock_guard<std::mutex> lock(r.mutex);
  if (!r.inited) return ESP_ERR_WIFI_NOT_INIT;
  if (!r.started) return ESP_ERR_WIFI_NOT_STARTED;
  if (r.link != Link::Up) return ESP_ERR_WIFI_NOT_CONNECT;
  *ap_info = r.ap;
  return ESP_OK;
}

}  // extern "C"
