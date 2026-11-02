#pragma once

#include <cstddef>

// What stands between the console queue and firmware's USB serial driver.
namespace preflash::usb {

bool installed();

// Hands bytes to usb_serial_jtag_read_bytes, waiting for room as the buffer firmware
// asked for fills, as a host waits for a device that isn't reading.
void feed(const char* data, size_t size);

// In the console, which holds the queue and knows about any REPL.
bool repl_has_port();
void wake_queue();

}  // namespace preflash::usb
