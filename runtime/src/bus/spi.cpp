#include <array>
#include <atomic>

#include "driver/spi_master.h"
#include "preflash/bus.h"

namespace {

std::array<std::atomic<bool>, SPI_HOST_MAX> g_started{};

bool usable(spi_host_device_t host) { return host == SPI2_HOST || host == SPI3_HOST; }

}  // namespace

namespace preflash {

bool spi_bus_started(int host) {
  return usable(static_cast<spi_host_device_t>(host)) && g_started[host];
}

}  // namespace preflash

extern "C" {

esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t* bus_config,
                             spi_dma_chan_t) {
  if (!usable(host_id) || !bus_config) return ESP_ERR_INVALID_ARG;
  return g_started[host_id].exchange(true) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t spi_bus_free(spi_host_device_t host_id) {
  if (!usable(host_id)) return ESP_ERR_INVALID_ARG;
  return g_started[host_id].exchange(false) ? ESP_OK : ESP_ERR_INVALID_STATE;
}

}  // extern "C"
