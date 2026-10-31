#include "nvs.h"

#include <cstdio>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "nvs_flash.h"
#include "preflash/fail.h"

namespace fs = std::filesystem;

namespace {

constexpr size_t kMaxNameLength = 15;

using Blob = std::vector<uint8_t>;

// Writes made through a handle wait here until nvs_commit; an empty optional is an erase.
struct Handle {
  std::string name;
  bool read_only;
  std::map<std::string, std::optional<Blob>> pending;
};

std::mutex g_mutex;
bool g_initialized = false;
uint32_t g_next_handle = 1;
std::map<uint32_t, Handle> g_handles;

fs::path root() {
  const char* state = std::getenv("PREFLASH_STATE");
  if (!state || !*state) {
    preflash_unsupported("nvs", "PREFLASH_STATE names no folder to keep saved state in");
  }
  return fs::path(state) / "nvs";
}

// Device names are case-sensitive and may hold any byte, but the host's file
// system may not be, so anything outside lower-case letters, digits, '_' and
// '-' is written as %XX.
std::string file_name(const std::string& name) {
  std::string out;
  for (unsigned char c : name) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-') {
      out += static_cast<char>(c);
    } else {
      char hex[4];
      std::snprintf(hex, sizeof hex, "%%%02X", c);
      out += hex;
    }
  }
  return out;
}

fs::path key_path(const Handle& h, const std::string& key) {
  return root() / file_name(h.name) / (file_name(key) + ".bin");
}

esp_err_t check_name(const char* name) {
  if (!name) return ESP_ERR_INVALID_ARG;
  const std::string s(name);
  if (s.empty()) return ESP_ERR_NVS_INVALID_NAME;
  if (s.size() > kMaxNameLength) return ESP_ERR_NVS_KEY_TOO_LONG;
  return ESP_OK;
}

std::optional<Blob> read_file(const fs::path& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  return Blob(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// Writes beside the target and renames, so a crash never leaves half a value.
bool write_file(const fs::path& path, const Blob& bytes) {
  const fs::path temp = path.string() + ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    // Closing flushes; a failed flush must not reach the rename.
    out.close();
    if (!out) return false;
  }
  std::error_code ec;
  fs::rename(temp, path, ec);
  return !ec;
}

Handle* find(nvs_handle_t handle) {
  auto it = g_handles.find(handle);
  return it == g_handles.end() ? nullptr : &it->second;
}

}  // namespace

extern "C" {

esp_err_t nvs_flash_init(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::error_code ec;
  fs::create_directories(root(), ec);
  if (ec) return ESP_FAIL;
  g_initialized = true;
  return ESP_OK;
}

esp_err_t nvs_flash_deinit(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized) return ESP_ERR_NVS_NOT_INITIALIZED;
  g_initialized = false;
  g_handles.clear();
  return ESP_OK;
}

esp_err_t nvs_flash_erase(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::error_code ec;
  fs::remove_all(root(), ec);
  if (ec) return ESP_FAIL;
  // As on a device, the partition is closed and must be initialised again.
  g_initialized = false;
  g_handles.clear();
  return ESP_OK;
}

esp_err_t nvs_open(const char* name, nvs_open_mode_t mode, nvs_handle_t* out_handle) {
  if (!out_handle) return ESP_ERR_INVALID_ARG;
  if (esp_err_t err = check_name(name)) return err;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_initialized) return ESP_ERR_NVS_NOT_INITIALIZED;
  const fs::path dir = root() / file_name(name);
  std::error_code ec;
  if (mode == NVS_READONLY) {
    if (!fs::is_directory(dir, ec)) return ESP_ERR_NVS_NOT_FOUND;
  } else {
    fs::create_directories(dir, ec);
    if (ec) return ESP_FAIL;
  }
  const uint32_t id = g_next_handle++;
  g_handles[id] = Handle{name, mode == NVS_READONLY, {}};
  *out_handle = id;
  return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_handles.erase(handle);
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char* key, const void* value, size_t length) {
  if (!value && length > 0) return ESP_ERR_INVALID_ARG;
  if (esp_err_t err = check_name(key)) return err;
  std::lock_guard<std::mutex> lock(g_mutex);
  Handle* h = find(handle);
  if (!h) return ESP_ERR_NVS_INVALID_HANDLE;
  if (h->read_only) return ESP_ERR_NVS_READ_ONLY;
  const auto* bytes = static_cast<const uint8_t*>(value);
  h->pending[key] = Blob(bytes, bytes + length);
  return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char* key, void* out_value, size_t* length) {
  if (!length) return ESP_ERR_INVALID_ARG;
  if (esp_err_t err = check_name(key)) return err;
  std::lock_guard<std::mutex> lock(g_mutex);
  Handle* h = find(handle);
  if (!h) return ESP_ERR_NVS_INVALID_HANDLE;
  std::optional<Blob> value;
  if (auto it = h->pending.find(key); it != h->pending.end()) {
    value = it->second;
  } else {
    value = read_file(key_path(*h, key));
  }
  if (!value) return ESP_ERR_NVS_NOT_FOUND;
  // A null buffer asks only for the size, as on a device.
  if (out_value) {
    if (*length < value->size()) {
      *length = value->size();
      return ESP_ERR_NVS_INVALID_LENGTH;
    }
    std::copy(value->begin(), value->end(), static_cast<uint8_t*>(out_value));
  }
  *length = value->size();
  return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char* key) {
  if (esp_err_t err = check_name(key)) return err;
  std::lock_guard<std::mutex> lock(g_mutex);
  Handle* h = find(handle);
  if (!h) return ESP_ERR_NVS_INVALID_HANDLE;
  if (h->read_only) return ESP_ERR_NVS_READ_ONLY;
  bool exists = false;
  if (auto it = h->pending.find(key); it != h->pending.end()) {
    exists = it->second.has_value();
  } else {
    std::error_code ec;
    exists = fs::exists(key_path(*h, key), ec);
  }
  if (!exists) return ESP_ERR_NVS_NOT_FOUND;
  h->pending[key] = std::nullopt;
  return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Handle* h = find(handle);
  if (!h) return ESP_ERR_NVS_INVALID_HANDLE;
  // A device accepts a commit on a read-only handle and does nothing.
  if (h->read_only) return ESP_OK;
  const fs::path dir = root() / file_name(h->name);
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) return ESP_FAIL;
  for (const auto& [key, value] : h->pending) {
    const fs::path path = dir / (file_name(key) + ".bin");
    if (value) {
      if (!write_file(path, *value)) return ESP_FAIL;
    } else {
      fs::remove(path, ec);
      if (ec) return ESP_FAIL;
    }
  }
  h->pending.clear();
  return ESP_OK;
}

}  // extern "C"
