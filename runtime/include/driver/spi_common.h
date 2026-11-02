#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  SPI1_HOST = 0,
  SPI2_HOST = 1,
  SPI3_HOST = 2,
  SPI_HOST_MAX,
} spi_host_device_t;

typedef enum {
  SPI_DMA_DISABLED = 0,
  SPI_DMA_CH_AUTO = 3,
} spi_common_dma_t;

#ifdef __cplusplus
typedef int spi_dma_chan_t;  // a bare int in C++, as in ESP-IDF
#else
typedef spi_common_dma_t spi_dma_chan_t;
#endif

typedef struct {
  union {
    int mosi_io_num;
    int data0_io_num;
  };
  union {
    int miso_io_num;
    int data1_io_num;
  };
  int sclk_io_num;
  union {
    int quadwp_io_num;
    int data2_io_num;
  };
  union {
    int quadhd_io_num;
    int data3_io_num;
  };
  int data4_io_num;
  int data5_io_num;
  int data6_io_num;
  int data7_io_num;
  bool data_io_default_level;
  int max_transfer_sz;
  uint32_t flags;
  int isr_cpu_id;
  int intr_flags;
} spi_bus_config_t;

// A bus only has to be started before a device can join it: the panel is the one thing
// on the host's SPI. SPI1 belongs to the flash, as on the device.
esp_err_t spi_bus_initialize(spi_host_device_t host_id, const spi_bus_config_t* bus_config,
                             spi_dma_chan_t dma_chan);
esp_err_t spi_bus_free(spi_host_device_t host_id);

#ifdef __cplusplus
}
#endif
