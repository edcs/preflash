// Which app slot is running, and whether a newer version exists. Installing one is left out:
// the check is the part every board can do safely.
#pragma once

#include "esp_err.h"

namespace update {

enum class Check { idle, running, current, newer, failed };

struct Status {
  char version[32] = "";
  char running_slot[17] = "";
  char spare_slot[17] = "";
  bool confirmed = false;  // the running app has told the bootloader it works

  Check check = Check::idle;
  char newest[32] = "";
  esp_err_t error = ESP_OK;
};

// Tells the bootloader this app came up, so that it won't roll back.
void start();
Status status();

// Asks for the newest version, on the network's task.
void check();

}  // namespace update
