#include "mishaps.h"

#include <cstdlib>

#include "esp_check.h"

namespace mishaps {
namespace {

struct Sensor {
  int reading;
};

// The volatile keeps the compiler from working out what this does and dropping it.
int read_sensor(const Sensor* volatile sensor) { return sensor->reading; }

}  // namespace

void null_read() {
  const int reading = read_sensor(nullptr);
  std::exit(reading);
}

void abort_now() { std::abort(); }

void failed_check() { ESP_ERROR_CHECK(ESP_ERR_INVALID_STATE); }

}  // namespace mishaps
