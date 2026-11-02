#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  ESP_LINE_ENDINGS_CRLF,
  ESP_LINE_ENDINGS_CR,
  ESP_LINE_ENDINGS_LF,
} esp_line_endings_t;

// The host's stdout already goes to the stream, so there is nothing to register. stdin
// stays the simulator's: firmware reads through usb_serial_jtag_read_bytes. The stream
// carries lines as they're written, so any other line ending stops the run.
esp_err_t usb_serial_jtag_vfs_register(void);
void usb_serial_jtag_vfs_use_driver(void);
void usb_serial_jtag_vfs_use_nonblocking(void);
void usb_serial_jtag_vfs_set_rx_line_endings(esp_line_endings_t mode);
void usb_serial_jtag_vfs_set_tx_line_endings(esp_line_endings_t mode);

#ifdef __cplusplus
}
#endif
