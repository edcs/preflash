// Ways to fail on purpose, for looking at what a crash leaves behind.
#pragma once

namespace mishaps {

// Reads through a null pointer.
void null_read();

// Calls abort(), as a failed assert does.
void abort_now();

// Fails an ESP_ERROR_CHECK.
void failed_check();

}  // namespace mishaps
