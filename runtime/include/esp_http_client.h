#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEFAULT_HTTP_BUF_SIZE (512)

typedef struct esp_http_client* esp_http_client_handle_t;
typedef struct esp_http_client_event* esp_http_client_event_handle_t;

typedef enum {
  HTTP_EVENT_ERROR = 0,
  HTTP_EVENT_ON_CONNECTED,
  HTTP_EVENT_HEADERS_SENT,
  HTTP_EVENT_HEADER_SENT = HTTP_EVENT_HEADERS_SENT,
  HTTP_EVENT_ON_HEADER,
  HTTP_EVENT_ON_DATA,
  HTTP_EVENT_ON_FINISH,
  HTTP_EVENT_DISCONNECTED,
  HTTP_EVENT_REDIRECT,
} esp_http_client_event_id_t;

typedef struct esp_http_client_event {
  esp_http_client_event_id_t event_id;
  esp_http_client_handle_t client;
  void* data;
  int data_len;
  void* user_data;
  char* header_key;
  char* header_value;
} esp_http_client_event_t;

typedef enum {
  HTTP_TRANSPORT_UNKNOWN = 0x0,
  HTTP_TRANSPORT_OVER_TCP,
  HTTP_TRANSPORT_OVER_SSL,
} esp_http_client_transport_t;

typedef enum {
  ESP_HTTP_CLIENT_TLS_VER_ANY = 0,
  ESP_HTTP_CLIENT_TLS_VER_TLS_1_2 = 0x1,
  ESP_HTTP_CLIENT_TLS_VER_TLS_1_3 = 0x2,
  ESP_HTTP_CLIENT_TLS_VER_MAX,
} esp_http_client_proto_ver_t;

typedef esp_err_t (*http_event_handle_cb)(esp_http_client_event_t* evt);

typedef enum {
  HTTP_METHOD_GET = 0,
  HTTP_METHOD_POST,
  HTTP_METHOD_PUT,
  HTTP_METHOD_PATCH,
  HTTP_METHOD_DELETE,
  HTTP_METHOD_HEAD,
  HTTP_METHOD_NOTIFY,
  HTTP_METHOD_SUBSCRIBE,
  HTTP_METHOD_UNSUBSCRIBE,
  HTTP_METHOD_OPTIONS,
  HTTP_METHOD_COPY,
  HTTP_METHOD_MOVE,
  HTTP_METHOD_LOCK,
  HTTP_METHOD_UNLOCK,
  HTTP_METHOD_PROPFIND,
  HTTP_METHOD_PROPPATCH,
  HTTP_METHOD_MKCOL,
  HTTP_METHOD_REPORT,
  HTTP_METHOD_MAX,
} esp_http_client_method_t;

typedef enum {
  HTTP_AUTH_TYPE_NONE = 0,
  HTTP_AUTH_TYPE_BASIC,
  HTTP_AUTH_TYPE_DIGEST,
} esp_http_client_auth_type_t;

// Same fields in the same order as ESP-IDF's, for the ones firmware commonly sets.
// Fields the stand-in has no use for are accepted and ignored: the Mac decides
// keep-alive and the network interface, and certificates come from its keychain.
typedef struct {
  const char* url;
  const char* host;
  int port;
  const char* username;
  const char* password;
  esp_http_client_auth_type_t auth_type;
  const char* path;
  const char* query;
  const char* cert_pem;
  size_t cert_len;
  const char* client_cert_pem;
  size_t client_cert_len;
  const char* client_key_pem;
  size_t client_key_len;
  const char* client_key_password;
  size_t client_key_password_len;
  esp_http_client_proto_ver_t tls_version;
  const char* user_agent;
  esp_http_client_method_t method;
  int timeout_ms;
  bool disable_auto_redirect;
  int max_redirection_count;
  int max_authorization_retries;
  http_event_handle_cb event_handler;
  esp_http_client_transport_t transport_type;
  int buffer_size;
  int buffer_size_tx;
  void* user_data;
  bool is_async;
  bool use_global_ca_store;
  bool skip_cert_common_name_check;
  const char* common_name;
  esp_err_t (*crt_bundle_attach)(void* conf);
  bool keep_alive_enable;
  int keep_alive_idle;
  int keep_alive_interval;
  int keep_alive_count;
  struct ifreq* if_name;
} esp_http_client_config_t;

#define ESP_ERR_HTTP_BASE (0x7000)
#define ESP_ERR_HTTP_MAX_REDIRECT (ESP_ERR_HTTP_BASE + 1)
#define ESP_ERR_HTTP_CONNECT (ESP_ERR_HTTP_BASE + 2)
#define ESP_ERR_HTTP_WRITE_DATA (ESP_ERR_HTTP_BASE + 3)
#define ESP_ERR_HTTP_FETCH_HEADER (ESP_ERR_HTTP_BASE + 4)
#define ESP_ERR_HTTP_INVALID_TRANSPORT (ESP_ERR_HTTP_BASE + 5)
#define ESP_ERR_HTTP_CONNECTING (ESP_ERR_HTTP_BASE + 6)
#define ESP_ERR_HTTP_EAGAIN (ESP_ERR_HTTP_BASE + 7)
#define ESP_ERR_HTTP_CONNECTION_CLOSED (ESP_ERR_HTTP_BASE + 8)

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t* config);
esp_err_t esp_http_client_perform(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_url(esp_http_client_handle_t client, const char* url);
esp_err_t esp_http_client_set_post_field(esp_http_client_handle_t client, const char* data,
                                         int len);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t client, const char* key,
                                     const char* value);
// Looks in the reply's headers. The value stays valid until the next request.
esp_err_t esp_http_client_get_header(esp_http_client_handle_t client, const char* key,
                                     char** value);
esp_err_t esp_http_client_delete_header(esp_http_client_handle_t client, const char* key);
esp_err_t esp_http_client_delete_all_headers(esp_http_client_handle_t client);
esp_err_t esp_http_client_set_method(esp_http_client_handle_t client,
                                     esp_http_client_method_t method);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t client, int timeout_ms);
esp_err_t esp_http_client_set_username(esp_http_client_handle_t client, const char* username);
esp_err_t esp_http_client_set_password(esp_http_client_handle_t client, const char* password);
esp_err_t esp_http_client_set_authtype(esp_http_client_handle_t client,
                                       esp_http_client_auth_type_t auth_type);
esp_err_t esp_http_client_get_user_data(esp_http_client_handle_t client, void** data);
esp_err_t esp_http_client_set_user_data(esp_http_client_handle_t client, void* data);

// Streaming use: open, fetch_headers, read until it returns 0, close. Opening
// with a body to send (write_len above zero) is not supported.
esp_err_t esp_http_client_open(esp_http_client_handle_t client, int write_len);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t client);
bool esp_http_client_is_chunked_response(esp_http_client_handle_t client);
int esp_http_client_read(esp_http_client_handle_t client, char* buffer, int len);
int esp_http_client_get_status_code(esp_http_client_handle_t client);
int64_t esp_http_client_get_content_length(esp_http_client_handle_t client);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t client);
esp_http_client_transport_t esp_http_client_get_transport_type(esp_http_client_handle_t client);
int esp_http_client_get_errno(esp_http_client_handle_t client);
esp_err_t esp_http_client_close(esp_http_client_handle_t client);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t client);

#ifdef __cplusplus
}
#endif
