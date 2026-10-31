#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "preflash/fail.h"
#include "preflash/net.h"

namespace {

constexpr const char* kTag = "http";

// A reader that stalls shouldn't make a slow download grow without limit.
constexpr size_t kMaxBuffered = 1 << 20;
constexpr int kDefaultTimeoutMs = 5000;

void init_curl_once() {
  static std::once_flag once;
  std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
}

bool iequals(const std::string& a, const char* b) {
  const size_t n = std::strlen(b);
  if (a.size() != n) return false;
  for (size_t i = 0; i < n; ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  }
  return true;
}

const char* method_name(esp_http_client_method_t m) {
  switch (m) {
    case HTTP_METHOD_GET: return "GET";
    case HTTP_METHOD_POST: return "POST";
    case HTTP_METHOD_PUT: return "PUT";
    case HTTP_METHOD_PATCH: return "PATCH";
    case HTTP_METHOD_DELETE: return "DELETE";
    case HTTP_METHOD_HEAD: return "HEAD";
    case HTTP_METHOD_NOTIFY: return "NOTIFY";
    case HTTP_METHOD_SUBSCRIBE: return "SUBSCRIBE";
    case HTTP_METHOD_UNSUBSCRIBE: return "UNSUBSCRIBE";
    case HTTP_METHOD_OPTIONS: return "OPTIONS";
    case HTTP_METHOD_COPY: return "COPY";
    case HTTP_METHOD_MOVE: return "MOVE";
    case HTTP_METHOD_LOCK: return "LOCK";
    case HTTP_METHOD_UNLOCK: return "UNLOCK";
    case HTTP_METHOD_PROPFIND: return "PROPFIND";
    case HTTP_METHOD_PROPPATCH: return "PROPPATCH";
    case HTTP_METHOD_MKCOL: return "MKCOL";
    case HTTP_METHOD_REPORT: return "REPORT";
    default: return "GET";
  }
}

esp_err_t map_error(CURLcode code) {
  switch (code) {
    case CURLE_OK: return ESP_OK;
    case CURLE_TOO_MANY_REDIRECTS: return ESP_ERR_HTTP_MAX_REDIRECT;
    case CURLE_UNSUPPORTED_PROTOCOL:
    case CURLE_URL_MALFORMAT: return ESP_ERR_HTTP_INVALID_TRANSPORT;
    case CURLE_OPERATION_TIMEDOUT: return ESP_ERR_HTTP_FETCH_HEADER;
    case CURLE_SEND_ERROR: return ESP_ERR_HTTP_WRITE_DATA;
    case CURLE_RECV_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE: return ESP_ERR_HTTP_FETCH_HEADER;
    default: return ESP_ERR_HTTP_CONNECT;
  }
}

}  // namespace

struct esp_http_client {
  esp_http_client_config_t config{};
  std::string url;
  std::string user_agent;
  std::string username;
  std::string password;
  std::string post_field;
  bool has_post_field = false;
  esp_http_client_method_t method = HTTP_METHOD_GET;
  esp_http_client_auth_type_t auth_type = HTTP_AUTH_TYPE_NONE;
  int timeout_ms = kDefaultTimeoutMs;
  void* user_data = nullptr;
  std::vector<std::pair<std::string, std::string>> request_headers;

  // One transfer at a time: set by open or perform and cleared by close.
  CURL* easy = nullptr;
  CURLM* multi = nullptr;
  curl_slist* header_list = nullptr;
  char error_text[CURL_ERROR_SIZE] = {};
  bool streaming = false;
  bool paused = false;
  bool done = false;
  CURLcode result = CURLE_OK;
  bool headers_done = false;
  int status = 0;
  int64_t content_length = -1;
  bool chunked = false;
  std::vector<std::pair<std::string, std::string>> reply_headers;
  std::string body;
  size_t body_read = 0;  // the part of body already handed out
  int64_t total_read = 0;
  bool announced = false;  // the connected event has been sent

  void emit(esp_http_client_event_id_t id, void* data = nullptr, int len = 0,
            char* key = nullptr, char* value = nullptr) {
    if (!config.event_handler) return;
    esp_http_client_event_t event{};
    event.event_id = id;
    event.client = this;
    event.data = data;
    event.data_len = len;
    event.user_data = config.user_data;
    event.header_key = key;
    event.header_value = value;
    config.event_handler(&event);
  }

  void reset_reply() {
    done = false;
    paused = false;
    result = CURLE_OK;
    headers_done = false;
    status = 0;
    content_length = -1;
    chunked = false;
    reply_headers.clear();
    body.clear();
    body_read = 0;
    total_read = 0;
    announced = false;
    error_text[0] = 0;
  }

  static size_t on_header(char* buffer, size_t size, size_t items, void* self) {
    auto* c = static_cast<esp_http_client*>(self);
    const size_t n = size * items;
    std::string line(buffer, n);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (line.rfind("HTTP/", 0) == 0) {
      // A new response, which on a redirect replaces the one before it.
      c->reply_headers.clear();
      c->content_length = -1;
      c->chunked = false;
      c->headers_done = false;
      const size_t space = line.find(' ');
      c->status = space == std::string::npos ? 0 : std::atoi(line.c_str() + space + 1);
      if (!c->announced) {
        c->announced = true;
        c->emit(HTTP_EVENT_ON_CONNECTED);
        c->emit(HTTP_EVENT_HEADERS_SENT);
      }
    } else if (line.empty()) {
      const bool redirect_followed = c->status >= 300 && c->status < 400 &&
                                     !c->config.disable_auto_redirect &&
                                     c->find_reply_header("location") != nullptr;
      if (c->status >= 100 && c->status < 200) {
        // an interim reply: the real one follows
      } else if (!redirect_followed) {
        c->headers_done = true;
      }
    } else {
      const size_t colon = line.find(':');
      if (colon != std::string::npos) {
        std::string key = line.substr(0, colon);
        std::string value = line.substr(colon + 1);
        value.erase(0, value.find_first_not_of(" \t"));
        if (iequals(key, "content-length")) c->content_length = std::atoll(value.c_str());
        if (iequals(key, "transfer-encoding") && value.find("chunked") != std::string::npos)
          c->chunked = true;
        c->emit(HTTP_EVENT_ON_HEADER, nullptr, 0, key.data(), value.data());
        c->reply_headers.emplace_back(std::move(key), std::move(value));
      }
    }
    return n;
  }

  static size_t on_body(char* data, size_t size, size_t items, void* self) {
    auto* c = static_cast<esp_http_client*>(self);
    const size_t n = size * items;
    if (!c->streaming) {
      c->total_read += static_cast<int64_t>(n);
      c->emit(HTTP_EVENT_ON_DATA, data, static_cast<int>(n));
      return n;
    }
    if (c->body.size() - c->body_read >= kMaxBuffered) {
      c->paused = true;
      return CURL_WRITEFUNC_PAUSE;
    }
    c->body.append(data, n);
    return n;
  }

  const std::string* find_reply_header(const char* key) const {
    for (const auto& h : reply_headers) {
      if (iequals(h.first, key)) return &h.second;
    }
    return nullptr;
  }

  // Builds the transfer. False if curl itself can't be set up.
  bool start(bool stream) {
    init_curl_once();
    reset_reply();
    streaming = stream;
    easy = curl_easy_init();
    multi = curl_multi_init();
    if (!easy || !multi) {
      stop();
      return false;
    }
    curl_easy_setopt(easy, CURLOPT_URL, url.c_str());
    curl_easy_setopt(easy, CURLOPT_ERRORBUFFER, error_text);
    curl_easy_setopt(easy, CURLOPT_HEADERFUNCTION, &esp_http_client::on_header);
    curl_easy_setopt(easy, CURLOPT_HEADERDATA, this);
    curl_easy_setopt(easy, CURLOPT_WRITEFUNCTION, &esp_http_client::on_body);
    curl_easy_setopt(easy, CURLOPT_WRITEDATA, this);
    curl_easy_setopt(easy, CURLOPT_NOSIGNAL, 1L);
    // The device speaks HTTP/1.1 only, and a reply's framing is read the same way here.
    curl_easy_setopt(easy, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
    curl_easy_setopt(easy, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(easy, CURLOPT_ACCEPT_ENCODING, nullptr);
    curl_easy_setopt(easy, CURLOPT_USERAGENT, user_agent.c_str());
    // The timeout is for a silent network, not for the whole download, as on the device.
    const long ms = timeout_ms > 0 ? timeout_ms : kDefaultTimeoutMs;
    curl_easy_setopt(easy, CURLOPT_CONNECTTIMEOUT_MS, ms);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(easy, CURLOPT_LOW_SPEED_TIME, std::max<long>(1, (ms + 999) / 1000));
    if (config.disable_auto_redirect) {
      curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 0L);
    } else {
      curl_easy_setopt(easy, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(easy, CURLOPT_MAXREDIRS,
                       config.max_redirection_count > 0 ? long{config.max_redirection_count} : 10L);
    }
    // Servers are checked against the Mac's own certificates, so crt_bundle_attach has
    // nothing to do. A project's own pinned certificate is not used.
    if (config.skip_cert_common_name_check) curl_easy_setopt(easy, CURLOPT_SSL_VERIFYHOST, 0L);
    if (!username.empty()) {
      curl_easy_setopt(easy, CURLOPT_USERNAME, username.c_str());
      curl_easy_setopt(easy, CURLOPT_PASSWORD, password.c_str());
      if (auth_type == HTTP_AUTH_TYPE_BASIC) curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
      if (auth_type == HTTP_AUTH_TYPE_DIGEST) curl_easy_setopt(easy, CURLOPT_HTTPAUTH, CURLAUTH_DIGEST);
    }
    for (const auto& h : request_headers) {
      header_list = curl_slist_append(header_list, (h.first + ": " + h.second).c_str());
    }
    // Stops curl from adding "Expect: 100-continue" the device never sends.
    header_list = curl_slist_append(header_list, "Expect:");
    curl_easy_setopt(easy, CURLOPT_HTTPHEADER, header_list);
    switch (method) {
      case HTTP_METHOD_GET: break;
      case HTTP_METHOD_HEAD: curl_easy_setopt(easy, CURLOPT_NOBODY, 1L); break;
      default: curl_easy_setopt(easy, CURLOPT_CUSTOMREQUEST, method_name(method)); break;
    }
    if (has_post_field || method == HTTP_METHOD_POST) {
      curl_easy_setopt(easy, CURLOPT_POSTFIELDSIZE, static_cast<long>(post_field.size()));
      curl_easy_setopt(easy, CURLOPT_POSTFIELDS, post_field.c_str());
    }
    curl_multi_add_handle(multi, easy);
    return true;
  }

  void stop() {
    if (multi && easy) curl_multi_remove_handle(multi, easy);
    if (easy) curl_easy_cleanup(easy);
    if (multi) curl_multi_cleanup(multi);
    if (header_list) curl_slist_free_all(header_list);
    easy = nullptr;
    multi = nullptr;
    header_list = nullptr;
  }

  // Lets the transfer move on, waiting a little for the socket. Sets done at the end.
  void pump() {
    if (done || !multi) return;
    if (paused && body.size() - body_read < kMaxBuffered) {
      paused = false;
      curl_easy_pause(easy, CURLPAUSE_CONT);
    }
    int running = 0;
    curl_multi_perform(multi, &running);
    if (running == 0) {
      int queued = 0;
      while (CURLMsg* msg = curl_multi_info_read(multi, &queued)) {
        if (msg->msg == CURLMSG_DONE) result = msg->data.result;
      }
      done = true;
      return;
    }
    // A paused transfer waits for the reader, so there is no socket to wait on.
    int fds = 0;
    curl_multi_poll(multi, nullptr, 0, paused ? 5 : 100, &fds);
  }

  void note_failure() {
    if (result == CURLE_OK) return;
    // The curl text names the cause, such as an untrusted certificate or a refused connection.
    ESP_LOGW(kTag, "request failed: %s", error_text[0] ? error_text : curl_easy_strerror(result));
  }
};

extern "C" {

esp_err_t esp_crt_bundle_attach(void*) { return ESP_OK; }
void esp_crt_bundle_detach(void*) {}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config) {
  if (!config || (!config->url && !config->host)) return nullptr;
  if (config->is_async) {
    preflash_unsupported("esp_http_client_init with is_async",
                         "asynchronous requests are not simulated");
  }
  auto* c = new esp_http_client;
  c->config = *config;
  c->config.url = c->config.host = c->config.path = c->config.query = nullptr;
  c->config.username = c->config.password = c->config.user_agent = c->config.cert_pem = nullptr;
  if (config->url) {
    c->url = config->url;
  } else {
    const bool tls = config->transport_type == HTTP_TRANSPORT_OVER_SSL;
    c->url = std::string(tls ? "https://" : "http://") + config->host;
    if (config->port > 0) c->url += ":" + std::to_string(config->port);
    c->url += config->path ? config->path : "/";
    if (config->query) c->url += std::string("?") + config->query;
  }
  c->user_agent = config->user_agent ? config->user_agent : "ESP32 HTTP Client/1.0";
  if (config->username) c->username = config->username;
  if (config->password) c->password = config->password;
  c->method = config->method;
  c->auth_type = config->auth_type;
  c->user_data = config->user_data;
  if (config->timeout_ms > 0) c->timeout_ms = config->timeout_ms;
  return c;
}

esp_err_t esp_http_client_perform(esp_http_client_handle_t client) {
  if (!client) return ESP_ERR_INVALID_ARG;
  if (client->easy) esp_http_client_close(client);
  if (!preflash::net_online()) {
    ESP_LOGW(kTag, "no route: the simulated network is offline");
    client->emit(HTTP_EVENT_ERROR);
    return ESP_ERR_HTTP_CONNECT;
  }
  if (!client->start(false)) return ESP_ERR_NO_MEM;
  while (!client->done) client->pump();
  const CURLcode result = client->result;
  client->note_failure();
  const int status = client->status;
  const int64_t length = client->content_length;
  const std::vector<std::pair<std::string, std::string>> headers = client->reply_headers;
  client->stop();
  // The reply stays readable after the transfer, as with the device's client.
  client->status = status;
  client->content_length = length;
  client->reply_headers = headers;
  if (result != CURLE_OK) {
    client->emit(HTTP_EVENT_ERROR);
    client->emit(HTTP_EVENT_DISCONNECTED);
    return map_error(result);
  }
  client->emit(HTTP_EVENT_ON_FINISH);
  client->emit(HTTP_EVENT_DISCONNECTED);
  return ESP_OK;
}

esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char* url) {
  if (!client || !url) return ESP_ERR_INVALID_ARG;
  client->url = url;
  return ESP_OK;
}

esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char* data,
                                         int len) {
  if (!client) return ESP_ERR_INVALID_ARG;
  client->has_post_field = data != nullptr;
  client->post_field.assign(data ? data : "", data ? static_cast<size_t>(len) : 0);
  return ESP_OK;
}

esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char* key,
                                     const char* value) {
  if (!client || !key || !value) return ESP_ERR_INVALID_ARG;
  esp_http_client_delete_header(client, key);
  client->request_headers.emplace_back(key, value);
  return ESP_OK;
}

esp_err_t esp_http_client_get_header(esp_http_client_handle_t client, const char* key,
                                     char** value) {
  if (!client || !key || !value) return ESP_ERR_INVALID_ARG;
  const std::string* found = client->find_reply_header(key);
  *value = found ? const_cast<char*>(found->c_str()) : nullptr;
  return ESP_OK;
}

esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char* key) {
  if (!client || !key) return ESP_ERR_INVALID_ARG;
  auto& h = client->request_headers;
  h.erase(std::remove_if(h.begin(), h.end(), [&](const auto& p) { return iequals(p.first, key); }),
          h.end());
  return ESP_OK;
}

esp_err_t esp_http_client_delete_all_headers(esp_http_client_handle_t client) {
  if (!client) return ESP_ERR_INVALID_ARG;
  client->request_headers.clear();
  return ESP_OK;
}

esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,
                                     esp_http_client_method_t method) {
  if (!client || method >= HTTP_METHOD_MAX) return ESP_ERR_INVALID_ARG;
  client->method = method;
  return ESP_OK;
}

esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms) {
  if (!client) return ESP_ERR_INVALID_ARG;
  client->timeout_ms = timeout_ms > 0 ? timeout_ms : kDefaultTimeoutMs;
  return ESP_OK;
}

esp_err_t esp_http_client_set_username(esp_http_client_handle_t client, const char* username) {
  if (!client) return ESP_ERR_INVALID_ARG;
  client->username = username ? username : "";
  return ESP_OK;
}

esp_err_t esp_http_client_set_password(esp_http_client_handle_t client, const char* password) {
  if (!client) return ESP_ERR_INVALID_ARG;
  client->password = password ? password : "";
  return ESP_OK;
}

esp_err_t esp_http_client_set_authtype(esp_http_client_handle_t client,
                                       esp_http_client_auth_type_t auth_type) {
  if (!client) return ESP_ERR_INVALID_ARG;
  client->auth_type = auth_type;
  return ESP_OK;
}

esp_err_t esp_http_client_get_user_data(esp_http_client_handle_t client, void** data) {
  if (!client || !data) return ESP_ERR_INVALID_ARG;
  *data = client->user_data;
  return ESP_OK;
}

esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void* data) {
  if (!client) return ESP_ERR_INVALID_ARG;
  client->user_data = data;
  return ESP_OK;
}

esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len) {
  if (!client) return ESP_ERR_INVALID_ARG;
  if (write_len > 0) {
    preflash_unsupported("esp_http_client_open with a body to write",
                         "sending a streamed body isn't simulated; use esp_http_client_set_post_field");
  }
  if (client->easy) esp_http_client_close(client);
  if (!preflash::net_online()) {
    ESP_LOGW(kTag, "no route: the simulated network is offline");
    return ESP_ERR_HTTP_CONNECT;
  }
  if (!client->start(true)) return ESP_ERR_NO_MEM;
  // Connects, sends the request and waits for the reply's headers, so a refused
  // connection or a bad certificate fails here as it does on the device.
  while (!client->headers_done && !client->done) client->pump();
  if (!client->headers_done) {
    const CURLcode result = client->result;
    client->note_failure();
    client->stop();
    return result == CURLE_OK ? ESP_ERR_HTTP_FETCH_HEADER : map_error(result);
  }
  return ESP_OK;
}

int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client) {
  if (!client || !client->easy) return -1;
  while (!client->headers_done && !client->done) client->pump();
  if (!client->headers_done) return -1;
  // Zero when the length isn't announced, as on the device.
  return client->content_length < 0 ? 0 : client->content_length;
}

bool esp_http_client_is_chunked_response(esp_http_client_handle_t client) {
  return client && client->chunked;
}

int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len) {
  if (!client || !buffer || len < 0) return -1;
  if (!client->easy) return -1;
  while (client->body.size() == client->body_read && !client->done) {
    if (!preflash::net_online()) return -1;
    client->pump();
  }
  const size_t available = client->body.size() - client->body_read;
  if (available == 0) {
    if (client->result != CURLE_OK) {
      client->note_failure();
      return -1;
    }
    return 0;
  }
  const size_t n = std::min(available, static_cast<size_t>(len));
  std::memcpy(buffer, client->body.data() + client->body_read, n);
  client->body_read += n;
  client->total_read += static_cast<int64_t>(n);
  // Drop what's been handed out once it's a good share of the buffer.
  if (client->body_read >= 65536) {
    client->body.erase(0, client->body_read);
    client->body_read = 0;
  }
  return static_cast<int>(n);
}

int esp_http_client_get_status_code(esp_http_client_handle_t client) {
  return client ? client->status : -1;
}

int64_t esp_http_client_get_content_length(esp_http_client_handle_t client) {
  return client ? client->content_length : -1;
}

bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client) {
  if (!client) return false;
  const bool drained = client->body.size() == client->body_read;
  return client->done && client->result == CURLE_OK && drained;
}

esp_http_client_transport_t esp_http_client_get_transport_type(esp_http_client_handle_t client) {
  if (!client) return HTTP_TRANSPORT_UNKNOWN;
  if (client->url.rfind("https://", 0) == 0) return HTTP_TRANSPORT_OVER_SSL;
  if (client->url.rfind("http://", 0) == 0) return HTTP_TRANSPORT_OVER_TCP;
  return HTTP_TRANSPORT_UNKNOWN;
}

int esp_http_client_get_errno(esp_http_client_handle_t client) {
  if (!client || !client->easy) return 0;
  long err = 0;
  curl_easy_getinfo(client->easy, CURLINFO_OS_ERRNO, &err);
  return static_cast<int>(err);
}

esp_err_t esp_http_client_close(esp_http_client_handle_t client) {
  if (!client) return ESP_ERR_INVALID_ARG;
  client->stop();
  return ESP_OK;
}

esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client) {
  if (!client) return ESP_ERR_INVALID_ARG;
  client->stop();
  delete client;
  return ESP_OK;
}

}  // extern "C"
