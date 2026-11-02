#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t tx_buffer_size;
  uint32_t rx_buffer_size;
} usb_serial_jtag_driver_config_t;

#define USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT() \
  (usb_serial_jtag_driver_config_t) { .tx_buffer_size = 256, .rx_buffer_size = 256 }

// Input from stdin, the control socket and the virtual serial port reaches
// usb_serial_jtag_read_bytes from here on, apart from "@sim " lines from the first two.
// Output joins the one serial stream, so stdout and this can't interleave inside a line
// any more than a device's can. The driver stays once it's in.
esp_err_t usb_serial_jtag_driver_install(usb_serial_jtag_driver_config_t* usb_serial_jtag_config);
esp_err_t usb_serial_jtag_driver_uninstall(void);
int usb_serial_jtag_read_bytes(void* buf, uint32_t length, TickType_t ticks_to_wait);
int usb_serial_jtag_write_bytes(const void* src, size_t size, TickType_t ticks_to_wait);
esp_err_t usb_serial_jtag_wait_tx_done(TickType_t ticks_to_wait);
bool usb_serial_jtag_is_connected(void);
bool usb_serial_jtag_is_driver_installed(void);

#ifdef __cplusplus
}
#endif
