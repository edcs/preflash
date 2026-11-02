#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>

#include "../os/ticks.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "preflash/fail.h"
#include "preflash/usb.h"

namespace {

constexpr uint32_t kSmallestRxBuffer = 65;  // ESP-IDF wants more than one 64-byte packet

struct Driver {
  std::atomic<bool> installed{false};
  std::mutex mutex;
  std::condition_variable readable;
  std::condition_variable room;
  std::deque<char> rx;
  size_t rx_capacity = 0;
};

Driver& driver() {
  static Driver* d = new Driver;  // never destroyed: tasks outlive static destructors
  return *d;
}

}  // namespace

namespace preflash::usb {

bool installed() { return driver().installed; }

void feed(const char* data, size_t size) {
  Driver& d = driver();
  while (size > 0) {
    std::unique_lock<std::mutex> lock(d.mutex);
    d.room.wait(lock, [&] { return d.rx.size() < d.rx_capacity; });
    const size_t part = std::min(size, d.rx_capacity - d.rx.size());
    d.rx.insert(d.rx.end(), data, data + part);
    lock.unlock();
    d.readable.notify_all();
    data += part;
    size -= part;
  }
}

}  // namespace preflash::usb

extern "C" {

esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t* usb_serial_jtag_config) {
  if (!usb_serial_jtag_config) return ESP_ERR_INVALID_ARG;
  Driver& d = driver();
  if (d.installed || preflash::usb::repl_has_port()) return ESP_ERR_INVALID_STATE;
  if (usb_serial_jtag_config->rx_buffer_size < kSmallestRxBuffer ||
      usb_serial_jtag_config->tx_buffer_size == 0) {
    return ESP_ERR_INVALID_ARG;
  }
  {
    std::lock_guard<std::mutex> lock(d.mutex);
    d.rx_capacity = usb_serial_jtag_config->rx_buffer_size;
  }
  d.installed = true;
  preflash::usb::wake_queue();
  return ESP_OK;
}

esp_err_t usb_serial_jtag_driver_uninstall(void) {
  preflash_unsupported("usb_serial_jtag_driver_uninstall", "a blocked read can't be released");
}

int usb_serial_jtag_read_bytes(void* buf, uint32_t length, TickType_t ticks_to_wait) {
  Driver& d = driver();
  if (!d.installed) preflash_unsupported("usb_serial_jtag_read_bytes", "the driver isn't installed");
  if (length == 0) return 0;
  std::unique_lock<std::mutex> lock(d.mutex);
  const auto any = [&] { return !d.rx.empty(); };
  if (ticks_to_wait == portMAX_DELAY) {
    d.readable.wait(lock, any);
  } else if (!d.readable.wait_for(lock, preflash::ticks_to_duration(ticks_to_wait), any)) {
    return 0;
  }
  const size_t got = std::min<size_t>(length, d.rx.size());
  std::copy_n(d.rx.begin(), got, static_cast<char*>(buf));
  d.rx.erase(d.rx.begin(), d.rx.begin() + static_cast<std::ptrdiff_t>(got));
  lock.unlock();
  d.room.notify_all();
  return static_cast<int>(got);
}

int usb_serial_jtag_write_bytes(const void* src, size_t size, TickType_t) {
  if (!src || size == 0 || !driver().installed) return 0;
  std::fwrite(src, 1, size, stdout);
  std::fflush(stdout);
  return static_cast<int>(size);
}

esp_err_t usb_serial_jtag_wait_tx_done(TickType_t) {
  if (!driver().installed) return ESP_ERR_INVALID_STATE;
  std::fflush(stdout);
  return ESP_OK;
}

bool usb_serial_jtag_is_connected(void) { return true; }

bool usb_serial_jtag_is_driver_installed(void) { return driver().installed; }

esp_err_t usb_serial_jtag_vfs_register(void) { return ESP_OK; }

void usb_serial_jtag_vfs_use_driver(void) {}

void usb_serial_jtag_vfs_use_nonblocking(void) {}

void usb_serial_jtag_vfs_set_rx_line_endings(esp_line_endings_t mode) {
  if (mode != ESP_LINE_ENDINGS_LF) {
    preflash_unsupported("usb_serial_jtag_vfs_set_rx_line_endings", "only LF is modelled");
  }
}

void usb_serial_jtag_vfs_set_tx_line_endings(esp_line_endings_t mode) {
  if (mode != ESP_LINE_ENDINGS_LF) {
    preflash_unsupported("usb_serial_jtag_vfs_set_tx_line_endings", "only LF is modelled");
  }
}

}  // extern "C"
