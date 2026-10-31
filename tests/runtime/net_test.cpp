#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_sntp.h"
#include "esp_wifi.h"
#include "mbedtls/base64.h"
#include "preflash/net.h"

namespace {

using namespace std::chrono_literals;

struct Seen {
  esp_event_base_t base;
  int32_t id;
  int reason;       // for a disconnect
  uint32_t number;  // for a finished scan
  uint32_t ip;      // for a new address
};

// Records every event the default loop delivers, in order.
class EventLog {
 public:
  static void on_event(void* self, esp_event_base_t base, int32_t id, void* data) {
    auto* log = static_cast<EventLog*>(self);
    Seen seen{base, id, 0, 0, 0};
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
      seen.reason = static_cast<wifi_event_sta_disconnected_t*>(data)->reason;
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
      seen.number = static_cast<wifi_event_sta_scan_done_t*>(data)->number;
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
      seen.ip = static_cast<ip_event_got_ip_t*>(data)->ip_info.ip.addr;
    }
    {
      std::lock_guard<std::mutex> lock(log->mutex_);
      log->seen_.push_back(seen);
    }
    log->changed_.notify_all();
  }

  bool wait_for(esp_event_base_t base, int32_t id, Seen* out = nullptr) {
    std::unique_lock<std::mutex> lock(mutex_);
    const auto find = [&]() -> const Seen* {
      for (const Seen& s : seen_) {
        if (s.base == base && s.id == id) return &s;
      }
      return nullptr;
    };
    if (!changed_.wait_for(lock, 3s, [&] { return find() != nullptr; })) return false;
    if (out) *out = *find();
    return true;
  }

  std::vector<Seen> snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return seen_;
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<Seen> seen_;
};

class Wifi : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(esp_event_loop_create_default(), ESP_OK);
    ASSERT_EQ(esp_netif_init(), ESP_OK);
    netif_ = esp_netif_create_default_wifi_sta();
    ASSERT_NE(netif_, nullptr);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ASSERT_EQ(esp_wifi_init(&cfg), ESP_OK);
    ASSERT_EQ(esp_wifi_set_mode(WIFI_MODE_STA), ESP_OK);
    ASSERT_EQ(esp_event_handler_register(ESP_EVENT_ANY_BASE, ESP_EVENT_ANY_ID,
                                         &EventLog::on_event, &log_),
              ESP_OK);
  }

  void TearDown() override {
    esp_wifi_stop();
    esp_wifi_deinit();
    esp_netif_destroy(netif_);
    esp_event_loop_delete_default();
    preflash::wifi_force_join_failure(false);
  }

  void join(const char* ssid, const char* password = "secret-password") {
    wifi_config_t config = {};
    std::memcpy(config.sta.ssid, ssid, std::strlen(ssid));
    std::memcpy(config.sta.password, password, std::strlen(password));
    config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    config.sta.pmf_cfg.capable = true;
    ASSERT_EQ(esp_wifi_set_config(WIFI_IF_STA, &config), ESP_OK);
    ASSERT_EQ(esp_wifi_connect(), ESP_OK);
  }

  EventLog log_;
  esp_netif_t* netif_ = nullptr;
};

}  // namespace

TEST_F(Wifi, ConnectPostsGotIp) {
  ASSERT_EQ(esp_wifi_start(), ESP_OK);
  join("Preflash Fake Home");
  Seen got_ip{};
  ASSERT_TRUE(log_.wait_for(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip));
  EXPECT_NE(got_ip.ip, 0u);

  // The order the firmware sees: started, connected, then an address.
  std::vector<int32_t> order;
  for (const Seen& s : log_.snapshot()) order.push_back(s.base == IP_EVENT ? 1000 + s.id : s.id);
  const std::vector<int32_t> expected = {WIFI_EVENT_STA_START, WIFI_EVENT_STA_CONNECTED,
                                         1000 + IP_EVENT_STA_GOT_IP};
  EXPECT_EQ(order, expected);

  esp_netif_ip_info_t info;
  ASSERT_EQ(esp_netif_get_ip_info(netif_, &info), ESP_OK);
  EXPECT_EQ(info.ip.addr, got_ip.ip);
  wifi_ap_record_t ap;
  ASSERT_EQ(esp_wifi_sta_get_ap_info(&ap), ESP_OK);
  EXPECT_STREQ(reinterpret_cast<const char*>(ap.ssid), "Preflash Fake Home");
}

TEST_F(Wifi, ScanListsSimulatedNetworks) {
  ASSERT_EQ(esp_wifi_start(), ESP_OK);
  ASSERT_EQ(esp_wifi_scan_start(nullptr, false), ESP_OK);
  Seen done{};
  ASSERT_TRUE(log_.wait_for(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, &done));
  ASSERT_GT(done.number, 1u);

  uint16_t count = 20;
  wifi_ap_record_t records[20];
  ASSERT_EQ(esp_wifi_scan_get_ap_records(&count, records), ESP_OK);
  ASSERT_EQ(count, done.number);
  bool has_open = false, has_secured = false;
  for (uint16_t i = 0; i < count; ++i) {
    const std::string ssid(reinterpret_cast<const char*>(records[i].ssid));
    EXPECT_EQ(ssid.rfind("Preflash Fake ", 0), 0u) << ssid;
    (records[i].authmode == WIFI_AUTH_OPEN ? has_open : has_secured) = true;
  }
  EXPECT_TRUE(has_open);
  EXPECT_TRUE(has_secured);

  // The list is handed over once, as on the device.
  count = 20;
  EXPECT_NE(esp_wifi_scan_get_ap_records(&count, records), ESP_OK);
}

TEST_F(Wifi, ForcedFailureDisconnectsWithAuthReason) {
  ASSERT_EQ(esp_wifi_start(), ESP_OK);
  preflash::wifi_force_join_failure(true);
  join("Preflash Fake Home");
  Seen gone{};
  ASSERT_TRUE(log_.wait_for(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &gone));
  EXPECT_EQ(gone.reason, WIFI_REASON_AUTH_FAIL);
  std::this_thread::sleep_for(300ms);
  for (const Seen& s : log_.snapshot()) {
    EXPECT_FALSE(s.base == IP_EVENT) << "an address came after a failed join";
    EXPECT_FALSE(s.base == WIFI_EVENT && s.id == WIFI_EVENT_STA_CONNECTED)
        << "connected after a failed join";
  }

  // Turning it off lets the next attempt through.
  preflash::wifi_force_join_failure(false);
  ASSERT_EQ(esp_wifi_connect(), ESP_OK);
  EXPECT_TRUE(log_.wait_for(IP_EVENT, IP_EVENT_STA_GOT_IP));
}

TEST_F(Wifi, DisconnectReportsAssocLeaveAndConnectNeedsStart) {
  EXPECT_EQ(esp_wifi_connect(), ESP_ERR_WIFI_NOT_STARTED);
  ASSERT_EQ(esp_wifi_start(), ESP_OK);
  EXPECT_EQ(esp_wifi_connect(), ESP_ERR_WIFI_SSID);
  join("Preflash Fake Guest", "");
  ASSERT_TRUE(log_.wait_for(IP_EVENT, IP_EVENT_STA_GOT_IP));
  ASSERT_EQ(esp_wifi_disconnect(), ESP_OK);
  Seen gone{};
  ASSERT_TRUE(log_.wait_for(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &gone));
  EXPECT_EQ(gone.reason, WIFI_REASON_ASSOC_LEAVE);
  EXPECT_EQ(esp_wifi_disconnect(), ESP_ERR_WIFI_NOT_CONNECT);
}

TEST_F(Wifi, ConnectRefusedWhileScanning) {
  ASSERT_EQ(esp_wifi_start(), ESP_OK);
  join("Preflash Fake Home");  // sets the config; the join itself isn't needed here
  ASSERT_TRUE(log_.wait_for(IP_EVENT, IP_EVENT_STA_GOT_IP));
  ASSERT_EQ(esp_wifi_disconnect(), ESP_OK);
  ASSERT_EQ(esp_wifi_scan_start(nullptr, false), ESP_OK);
  EXPECT_EQ(esp_wifi_connect(), ESP_ERR_WIFI_STATE);
  ASSERT_EQ(esp_wifi_scan_stop(), ESP_OK);
  EXPECT_EQ(esp_wifi_connect(), ESP_OK);
}

TEST(Events, HandlersRunInPostOrderAndUnregister) {
  ASSERT_EQ(esp_event_loop_create_default(), ESP_OK);
  EXPECT_EQ(esp_event_loop_create_default(), ESP_ERR_INVALID_STATE);
  static EventLog log;
  ASSERT_EQ(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventLog::on_event, &log),
            ESP_OK);
  for (int32_t id : {WIFI_EVENT_STA_START, WIFI_EVENT_STA_STOP, WIFI_EVENT_SCAN_DONE}) {
    wifi_event_sta_scan_done_t data{};
    ASSERT_EQ(esp_event_post(WIFI_EVENT, id, &data, sizeof data, portMAX_DELAY), ESP_OK);
  }
  ASSERT_TRUE(log.wait_for(WIFI_EVENT, WIFI_EVENT_SCAN_DONE));
  const auto seen = log.snapshot();
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[0].id, WIFI_EVENT_STA_START);
  EXPECT_EQ(seen[1].id, WIFI_EVENT_STA_STOP);
  EXPECT_EQ(seen[2].id, WIFI_EVENT_SCAN_DONE);
  EXPECT_EQ(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventLog::on_event),
            ESP_OK);
  EXPECT_EQ(esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, &EventLog::on_event),
            ESP_ERR_NOT_FOUND);
  ASSERT_EQ(esp_event_loop_delete_default(), ESP_OK);
}

namespace {

struct SntpHits {
  std::atomic<int> count{0};
  std::atomic<long> seconds{0};
};
SntpHits g_hits;

void on_sync(struct timeval* tv) {
  g_hits.seconds = static_cast<long>(tv->tv_sec);
  ++g_hits.count;
}

bool wait_sync(std::chrono::milliseconds limit) {
  const auto end = std::chrono::steady_clock::now() + limit;
  while (std::chrono::steady_clock::now() < end) {
    if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

}  // namespace

TEST(Sntp, SyncsFromTheMacClock) {
  g_hits.count = 0;
  g_hits.seconds = 0;
  sntp_set_time_sync_notification_cb(on_sync);
  esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
  esp_sntp_setservername(0, "pool.ntp.org");
  EXPECT_STREQ(esp_sntp_getservername(0), "pool.ntp.org");
  EXPECT_EQ(sntp_get_sync_status(), SNTP_SYNC_STATUS_RESET);

  timeval before, after;
  gettimeofday(&before, nullptr);
  esp_sntp_init();
  EXPECT_TRUE(esp_sntp_enabled());
  ASSERT_TRUE(wait_sync(3s));
  gettimeofday(&after, nullptr);
  // Reading COMPLETED reset it.
  EXPECT_EQ(sntp_get_sync_status(), SNTP_SYNC_STATUS_RESET);

  // The clock firmware sees is the Mac's, running on without a step.
  EXPECT_GE(g_hits.count.load(), 1);
  EXPECT_GE(g_hits.seconds.load(), static_cast<long>(before.tv_sec));
  EXPECT_LE(g_hits.seconds.load(), static_cast<long>(after.tv_sec));
  EXPECT_LE(after.tv_sec - before.tv_sec, 5);

  esp_sntp_stop();
  EXPECT_FALSE(esp_sntp_enabled());
  sntp_set_time_sync_notification_cb(nullptr);
}

TEST(Sntp, OfflineNeverSyncsUntilOnline) {
  preflash::net_set_online(false);
  esp_sntp_init();
  EXPECT_FALSE(wait_sync(400ms));
  preflash::net_set_online(true);
  EXPECT_TRUE(wait_sync(3s));
  esp_sntp_stop();
}

namespace {

// Serves plain HTTP on a loopback port, one connection at a time, until stopped.
class LocalServer {
 public:
  LocalServer() {
    fd_ = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr);
    listen(fd_, 8);
    socklen_t len = sizeof addr;
    getsockname(fd_, reinterpret_cast<sockaddr*>(&addr), &len);
    port_ = ntohs(addr.sin_port);
    thread_ = std::thread([this] { serve(); });
  }
  ~LocalServer() {
    stop_ = true;
    shutdown(fd_, SHUT_RDWR);
    close(fd_);
    thread_.join();
  }
  std::string url(const char* path) const {
    return "http://127.0.0.1:" + std::to_string(port_) + path;
  }
  int port() const { return port_; }
  std::string last_request() {
    std::lock_guard<std::mutex> lock(mutex_);
    return last_request_;
  }

 private:
  void serve() {
    while (!stop_) {
      const int client = accept(fd_, nullptr, nullptr);
      if (client < 0) return;
      // A client that speaks something else, such as TLS, gets dropped instead of waited on.
      timeval patience{0, 300000};
      setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof patience);
      std::string request;
      char buf[1024];
      while (request.find("\r\n\r\n") == std::string::npos) {
        const ssize_t n = recv(client, buf, sizeof buf, 0);
        if (n <= 0) break;
        request.append(buf, static_cast<size_t>(n));
      }
      {
        std::lock_guard<std::mutex> lock(mutex_);
        last_request_ = request;
      }
      std::string reply;
      if (request.rfind("GET /big", 0) == 0) {
        const std::string body(300000, 'x');
        reply = "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(body.size()) +
                "\r\nConnection: close\r\n\r\n" + body;
      } else if (request.rfind("GET /missing", 0) == 0) {
        reply = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
      } else if (request.rfind("GET /moved", 0) == 0) {
        reply = "HTTP/1.1 302 Found\r\nLocation: /hello\r\nContent-Length: 0\r\n"
                "Connection: close\r\n\r\n";
      } else {
        const std::string body = "hello from the test server";
        reply = "HTTP/1.1 200 OK\r\nX-Preflash: yes\r\nContent-Length: " +
                std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
      }
      size_t sent = 0;
      while (sent < reply.size()) {
        const ssize_t n = send(client, reply.data() + sent, reply.size() - sent, 0);
        if (n <= 0) break;
        sent += static_cast<size_t>(n);
      }
      close(client);
    }
  }

  int fd_ = -1;
  int port_ = 0;
  std::atomic<bool> stop_{false};
  std::thread thread_;
  std::mutex mutex_;
  std::string last_request_;
};

struct Reply {
  esp_err_t open = ESP_FAIL;
  int64_t headers = -2;
  int status = 0;
  std::string body;
};

// The same calls the firmware's https_get makes.
Reply get(const std::string& url, bool* got_header = nullptr) {
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.timeout_ms = 3000;
  cfg.user_agent = "preflash-test";
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  Reply reply;
  reply.open = esp_http_client_open(client, 0);
  if (reply.open == ESP_OK) {
    reply.headers = esp_http_client_fetch_headers(client);
    reply.status = esp_http_client_get_status_code(client);
    char* value = nullptr;
    if (got_header && esp_http_client_get_header(client, "X-Preflash", &value) == ESP_OK)
      *got_header = value && std::string(value) == "yes";
    char buf[256];
    for (;;) {
      const int n = esp_http_client_read(client, buf, sizeof buf);
      if (n <= 0) break;
      reply.body.append(buf, static_cast<size_t>(n));
    }
  }
  esp_http_client_close(client);
  esp_http_client_cleanup(client);
  return reply;
}

}  // namespace

TEST(Http, GetsFromALocalServer) {
  LocalServer server;
  bool header_seen = false;
  const Reply reply = get(server.url("/hello"), &header_seen);
  EXPECT_EQ(reply.open, ESP_OK);
  EXPECT_EQ(reply.status, 200);
  EXPECT_EQ(reply.headers, 26);
  EXPECT_EQ(reply.body, "hello from the test server");
  EXPECT_TRUE(header_seen);
  EXPECT_NE(server.last_request().find("User-Agent: preflash-test"), std::string::npos);
}

TEST(Http, ReportsStatusAndFollowsRedirects) {
  LocalServer server;
  EXPECT_EQ(get(server.url("/missing")).status, 404);
  const Reply moved = get(server.url("/moved"));
  EXPECT_EQ(moved.status, 200);
  EXPECT_EQ(moved.body, "hello from the test server");
}

TEST(Http, ReadsABodyLargerThanOneBufferAndPerformStreamsToTheHandler) {
  LocalServer server;
  EXPECT_EQ(get(server.url("/big")).body.size(), 300000u);

  std::string collected;
  esp_http_client_config_t cfg = {};
  const std::string url = server.url("/hello");
  cfg.url = url.c_str();
  cfg.user_data = &collected;
  cfg.event_handler = [](esp_http_client_event_t* e) -> esp_err_t {
    if (e->event_id == HTTP_EVENT_ON_DATA)
      static_cast<std::string*>(e->user_data)->append(static_cast<char*>(e->data),
                                                       static_cast<size_t>(e->data_len));
    return ESP_OK;
  };
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  ASSERT_EQ(esp_http_client_perform(client), ESP_OK);
  EXPECT_EQ(esp_http_client_get_status_code(client), 200);
  EXPECT_EQ(collected, "hello from the test server");
  esp_http_client_cleanup(client);
}

TEST(Http, OfflineFails) {
  LocalServer server;
  preflash::net_set_online(false);
  const Reply reply = get(server.url("/hello"));
  EXPECT_EQ(reply.open, ESP_ERR_HTTP_CONNECT);
  esp_http_client_config_t cfg = {};
  const std::string url = server.url("/hello");
  cfg.url = url.c_str();
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  EXPECT_EQ(esp_http_client_perform(client), ESP_ERR_HTTP_CONNECT);
  esp_http_client_cleanup(client);
  preflash::net_set_online(true);
  EXPECT_EQ(get(server.url("/hello")).status, 200);
}

TEST(Http, RefusedConnectionFailsToOpen) {
  int port;
  {
    LocalServer server;  // its port is closed again once it goes
    port = server.port();
  }
  const Reply reply = get("http://127.0.0.1:" + std::to_string(port) + "/");
  EXPECT_EQ(reply.open, ESP_ERR_HTTP_CONNECT);
}

TEST(Http, HttpsToAPlainServerFailsTheHandshake) {
  LocalServer server;
  const Reply reply = get("https://127.0.0.1:" + std::to_string(server.port()) + "/");
  EXPECT_NE(reply.open, ESP_OK);
}

TEST(Base64, MatchesMbedtlsForTheSizesTheConsoleUses) {
  unsigned char out[16];
  size_t len = 0;
  const unsigned char data[] = {'f', 'o', 'o', 'b', 'a'};
  ASSERT_EQ(mbedtls_base64_encode(out, sizeof out, &len, data, 5), 0);
  EXPECT_EQ(std::string(reinterpret_cast<char*>(out), len), "Zm9vYmE=");
  EXPECT_EQ(out[len], '\0');
  // The text fits but its NUL doesn't, which mbedtls refuses too, and says what it needs.
  unsigned char tight[8];
  EXPECT_EQ(mbedtls_base64_encode(tight, sizeof tight, &len, data, 5),
            MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL);
  EXPECT_EQ(len, 8u);
}

TEST(Ota, ReportsNoUpdateAvailable) {
  const esp_partition_t* running = esp_ota_get_running_partition();
  const esp_partition_t* spare = esp_ota_get_next_update_partition(nullptr);
  ASSERT_NE(running, nullptr);
  ASSERT_NE(spare, nullptr);
  EXPECT_NE(running, spare);

  esp_ota_img_states_t state;
  ASSERT_EQ(esp_ota_get_state_partition(running, &state), ESP_OK);
  EXPECT_EQ(state, ESP_OTA_IMG_VALID);
  EXPECT_EQ(esp_ota_get_last_invalid_partition(), nullptr);
  esp_app_desc_t desc;
  EXPECT_EQ(esp_ota_get_partition_description(running, &desc), ESP_OK);
  EXPECT_EQ(esp_ota_get_partition_description(spare, &desc), ESP_ERR_NOT_FOUND);

  // Asking for the message before OTA is reached would print it for every project.
  testing::internal::CaptureStdout();
  esp_https_ota_handle_t handle = reinterpret_cast<esp_https_ota_handle_t>(1);
  esp_http_client_config_t http = {};
  http.url = "http://127.0.0.1:1/firmware.bin";
  esp_https_ota_config_t config = {};
  config.http_config = &http;
  EXPECT_NE(esp_https_ota_begin(&config, &handle), ESP_OK);
  EXPECT_EQ(handle, nullptr);
  EXPECT_NE(esp_https_ota_begin(&config, &handle), ESP_OK);
  EXPECT_NE(esp_ota_set_boot_partition(spare), ESP_OK);
  const std::string logged = testing::internal::GetCapturedStdout();
  const size_t first = logged.find("not simulated");
  ASSERT_NE(first, std::string::npos) << logged;
  EXPECT_EQ(logged.find("not simulated", first + 1), std::string::npos) << "logged more than once";
}
