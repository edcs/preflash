// Restarts once, leaving a marker in PREFLASH_STATE; the second run reports it.
#include <cstdio>
#include <cstdlib>
#include <string>

#include "esp_system.h"

int main() {
  const char* state = std::getenv("PREFLASH_STATE");
  if (!state) return 2;
  const std::string marker = std::string(state) + "/restarted";
  if (FILE* seen = std::fopen(marker.c_str(), "r")) {
    std::fclose(seen);
    std::printf("second run: state=%s reason=%d\n", state, static_cast<int>(esp_reset_reason()));
    return 0;
  }
  FILE* made = std::fopen(marker.c_str(), "w");
  if (!made) return 3;
  std::fclose(made);
  esp_restart();
}
