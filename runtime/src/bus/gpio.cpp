#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>

#include "driver/gpio.h"
#include "preflash/bus.h"
#include "preflash/crash.h"
#include "preflash/fail.h"
#include "preflash/trace.h"

namespace {

constexpr int kInputBit = 1;
constexpr int kOutputBit = 2;
constexpr int kOpenDrainBit = 4;

struct Pin {
  bool input = false;       // firmware can read the pad
  bool output = false;      // the output driver is on
  bool open_drain = false;  // a high latch lets go of the line instead of driving it
  bool latch = false;       // what gpio_set_level last asked for
  bool outside = false;     // what something off the chip drives, seen when output is off
  bool driven = false;      // something off the chip has driven the pin
  bool pull_up = false;     // holds an undriven pin high
  bool held = false;        // the pad keeps its level whatever the rest of this says
  int epoch = 0;            // how many times the direction was set
  bool level = false;       // what's on the pad
  gpio_int_type_t intr_type = GPIO_INTR_DISABLE;
  bool intr_enabled = false;
  gpio_isr_t handler = nullptr;
  void* handler_arg = nullptr;
};

std::mutex g_mutex;
std::map<int, Pin> g_pins;

// A level change that matches a pin's interrupt type queues the pin here, under g_mutex, and
// returns: the thread standing in for the ISR calls the handler later with g_mutex released,
// so a handler can call back into GPIO and a slow one never holds up gpio_write.
bool g_service = false;
bool g_isr_thread = false;
std::deque<int> g_events;
std::condition_variable g_wake;

bool exists(gpio_num_t pin) {
  return pin >= GPIO_NUM_0 && pin < GPIO_NUM_MAX && (pin <= GPIO_NUM_21 || pin >= GPIO_NUM_26);
}

// Whether moving to `level` is what the pin's interrupt type waits for. A level type counts
// the move into its level, not the time spent there, or a held key would starve the firmware.
bool triggers(gpio_int_type_t type, bool level) {
  switch (type) {
    case GPIO_INTR_POSEDGE:
    case GPIO_INTR_HIGH_LEVEL:
      return level;
    case GPIO_INTR_NEGEDGE:
    case GPIO_INTR_LOW_LEVEL:
      return !level;
    case GPIO_INTR_ANYEDGE:
      return true;
    default:
      return false;
  }
}

void settle(int num, Pin* pin) {
  if (pin->held) return;
  const bool before = pin->level;
  const bool driving = pin->output && !(pin->open_drain && pin->latch);
  if (driving) {
    pin->level = pin->latch;
  } else {
    pin->level = pin->driven ? pin->outside : pin->pull_up;
  }
  if (pin->level != before && g_service && pin->intr_enabled && pin->handler &&
      triggers(pin->intr_type, pin->level)) {
    g_events.push_back(num);
    g_wake.notify_one();
  }
}

// Arming a level interrupt while the pad is already at its level fires at once, as the
// hardware does: a key held through a restart still reaches firmware that waits on LOW_LEVEL.
void arm(int num, Pin* pin) {
  pin->intr_enabled = true;
  const bool level_type = pin->intr_type == GPIO_INTR_LOW_LEVEL || pin->intr_type == GPIO_INTR_HIGH_LEVEL;
  if (g_service && pin->handler && level_type && triggers(pin->intr_type, pin->level)) {
    g_events.push_back(num);
    g_wake.notify_one();
  }
}

void run_isr() {
  preflash::crash::enter_task("gpio_isr");
  std::unique_lock<std::mutex> lock(g_mutex);
  for (;;) {
    g_wake.wait(lock, [] { return !g_events.empty(); });
    // Once firmware has crashed, its handlers stay quiet like its other threads.
    if (preflash::crash::happened()) return;
    const int num = g_events.front();
    g_events.pop_front();
    const Pin& pin = g_pins[num];
    if (!g_service || !pin.intr_enabled || !pin.handler) continue;
    const gpio_isr_t handler = pin.handler;
    void* const arg = pin.handler_arg;
    lock.unlock();
    handler(arg);
    lock.lock();
  }
}

const char* direction_name(gpio_mode_t mode) {
  if (mode & kOpenDrainBit) return "open drain";
  if (mode & kOutputBit) return "output";
  return mode & kInputBit ? "input" : "off";
}

void trace_pin(gpio_num_t pin, const char* op, const char* data) {
  preflash::trace::note(preflash::trace::Stream::Gpio, ("gpio" + std::to_string(pin)).c_str(), op,
                        data);
}

bool valid_mask(uint64_t mask) {
  if (mask == 0) return false;
  for (int bit = 0; bit < 64; ++bit) {
    if ((mask >> bit & 1) && !exists(static_cast<gpio_num_t>(bit))) return false;
  }
  return true;
}

esp_err_t set_pull(gpio_num_t gpio_num, bool up) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  Pin& pin = g_pins[gpio_num];
  pin.pull_up = up;
  settle(gpio_num, &pin);
  return ESP_OK;
}

}  // namespace

namespace preflash {

void gpio_write(int pin, bool level) {
  std::lock_guard<std::mutex> lock(g_mutex);
  Pin& driven = g_pins[pin];
  driven.outside = level;
  driven.driven = true;
  settle(pin, &driven);
}

bool gpio_read(int pin) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_pins[pin].level;
}

bool gpio_is_output(int pin) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_pins[pin].output;
}

int gpio_direction_epoch(int pin) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_pins[pin].epoch;
}

bool gpio_is_held(int pin) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_pins[pin].held;
}

}  // namespace preflash

extern "C" {

esp_err_t gpio_config(const gpio_config_t* config) {
  if (!config || !valid_mask(config->pin_bit_mask)) return ESP_ERR_INVALID_ARG;
  if (config->intr_type < GPIO_INTR_DISABLE || config->intr_type >= GPIO_INTR_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  for (int bit = 0; bit < 64; ++bit) {
    if (!(config->pin_bit_mask >> bit & 1)) continue;
    const auto pin = static_cast<gpio_num_t>(bit);
    gpio_set_direction(pin, config->mode);
    set_pull(pin, config->pull_up_en == GPIO_PULLUP_ENABLE);
    gpio_set_intr_type(pin, config->intr_type);
    if (config->intr_type != GPIO_INTR_DISABLE) {
      gpio_intr_enable(pin);
    } else {
      gpio_intr_disable(pin);
    }
  }
  return ESP_OK;
}

// A reset pin reads what's outside it, so that a resistor on the board can decide.
esp_err_t gpio_reset_pin(gpio_num_t gpio_num) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  set_pull(gpio_num, false);
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    Pin& pin = g_pins[gpio_num];
    pin.intr_type = GPIO_INTR_DISABLE;
    pin.intr_enabled = false;
  }
  return gpio_set_direction(gpio_num, GPIO_MODE_DISABLE);
}

esp_err_t gpio_set_direction(gpio_num_t gpio_num, gpio_mode_t mode) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  Pin& pin = g_pins[gpio_num];
  pin.epoch++;
  pin.input = (mode & kInputBit) != 0;
  pin.output = (mode & kOutputBit) != 0;
  pin.open_drain = (mode & kOpenDrainBit) != 0;
  settle(gpio_num, &pin);
  if (preflash::trace::on()) trace_pin(gpio_num, "direction", direction_name(mode));
  return ESP_OK;
}

esp_err_t gpio_set_level(gpio_num_t gpio_num, uint32_t level) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  Pin& pin = g_pins[gpio_num];
  pin.latch = level != 0;
  settle(gpio_num, &pin);
  if (preflash::trace::on()) trace_pin(gpio_num, "level", level ? "high" : "low");
  return ESP_OK;
}

int gpio_get_level(gpio_num_t gpio_num) {
  if (!exists(gpio_num)) return 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  const Pin& pin = g_pins[gpio_num];
  return pin.input && pin.level ? 1 : 0;
}

esp_err_t gpio_set_pull_mode(gpio_num_t gpio_num, gpio_pull_mode_t pull) {
  if (pull < GPIO_PULLUP_ONLY || pull > GPIO_FLOATING) return ESP_ERR_INVALID_ARG;
  return set_pull(gpio_num, pull == GPIO_PULLUP_ONLY || pull == GPIO_PULLUP_PULLDOWN);
}

esp_err_t gpio_pullup_en(gpio_num_t gpio_num) { return set_pull(gpio_num, true); }

esp_err_t gpio_pullup_dis(gpio_num_t gpio_num) { return set_pull(gpio_num, false); }

// A pull-down reads the same as nothing at all here, because an undriven pin is low.
esp_err_t gpio_pulldown_en(gpio_num_t gpio_num) { return set_pull(gpio_num, false); }

esp_err_t gpio_pulldown_dis(gpio_num_t gpio_num) {
  return exists(gpio_num) ? ESP_OK : ESP_ERR_INVALID_ARG;
}

esp_err_t gpio_hold_en(gpio_num_t gpio_num) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pins[gpio_num].held = true;
  return ESP_OK;
}

esp_err_t gpio_hold_dis(gpio_num_t gpio_num) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  Pin& pin = g_pins[gpio_num];
  pin.held = false;
  settle(gpio_num, &pin);
  return ESP_OK;
}

esp_err_t gpio_set_intr_type(gpio_num_t gpio_num, gpio_int_type_t intr_type) {
  if (!exists(gpio_num) || intr_type < GPIO_INTR_DISABLE || intr_type >= GPIO_INTR_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pins[gpio_num].intr_type = intr_type;
  return ESP_OK;
}

esp_err_t gpio_intr_enable(gpio_num_t gpio_num) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  arm(gpio_num, &g_pins[gpio_num]);
  return ESP_OK;
}

esp_err_t gpio_intr_disable(gpio_num_t gpio_num) {
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_pins[gpio_num].intr_enabled = false;
  return ESP_OK;
}

esp_err_t gpio_install_isr_service(int) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_service) return ESP_ERR_INVALID_STATE;
  if (!g_isr_thread) {
    try {
      std::thread(run_isr).detach();
    } catch (const std::system_error&) {
      return ESP_ERR_NO_MEM;
    }
    g_isr_thread = true;
  }
  g_service = true;
  return ESP_OK;
}

void gpio_uninstall_isr_service(void) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_service = false;
  g_events.clear();
  for (auto& entry : g_pins) {
    entry.second.handler = nullptr;
    entry.second.handler_arg = nullptr;
  }
}

esp_err_t gpio_isr_handler_add(gpio_num_t gpio_num, gpio_isr_t isr_handler, void* args) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_service) return ESP_ERR_INVALID_STATE;
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  Pin& pin = g_pins[gpio_num];
  pin.handler = isr_handler;
  pin.handler_arg = args;
  arm(gpio_num, &pin);
  return ESP_OK;
}

esp_err_t gpio_isr_handler_remove(gpio_num_t gpio_num) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_service) return ESP_ERR_INVALID_STATE;
  if (!exists(gpio_num)) return ESP_ERR_INVALID_ARG;
  Pin& pin = g_pins[gpio_num];
  pin.handler = nullptr;
  pin.handler_arg = nullptr;
  pin.intr_enabled = false;
  return ESP_OK;
}

esp_err_t gpio_isr_register(void (*)(void*), void*, int, gpio_isr_handle_t*) {
  preflash_unsupported("gpio_isr_register",
                       "a GPIO-wide interrupt handler isn't simulated; use gpio_install_isr_service "
                       "and gpio_isr_handler_add");
}

}  // extern "C"
