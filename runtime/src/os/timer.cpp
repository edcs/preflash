#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <new>
#include <set>
#include <thread>
#include <utility>

#include "esp_timer.h"

namespace {

// As the device's esp_timer_impl_get_min_period_us.
constexpr uint64_t kMinPeriodUs = 50;

// A function-local static, so a global constructor elsewhere can call
// esp_timer_get_time() before the start time exists.
std::chrono::steady_clock::time_point start_time() {
  static const auto start = std::chrono::steady_clock::now();
  return start;
}

struct Earliest {
  bool operator()(const esp_timer* a, const esp_timer* b) const;
};

// Every timer shares one thread, so a slow callback holds up the rest, as on the
// device's timer task. The lock is free while a callback runs, so a callback may
// stop or delete its own timer, or start another.
struct TimerTask {
  std::mutex mutex;
  std::condition_variable changed;
  std::multiset<esp_timer*, Earliest> armed;
  bool running = false;
};

// Never destroyed: firmware threads outlive static destruction.
TimerTask& timer_task() {
  static TimerTask* task = new TimerTask;
  return *task;
}

}  // namespace

struct esp_timer {
  esp_timer_cb_t callback;
  void* arg;
  bool skip_unhandled_events;
  int64_t alarm = 0;
  int64_t period = 0;
  bool is_armed = false;
};

namespace {

bool Earliest::operator()(const esp_timer* a, const esp_timer* b) const {
  return a->alarm < b->alarm;
}

void disarm(TimerTask& task, esp_timer* timer) {
  // By pointer: the set orders on the alarm, which many timers can share.
  const auto [first, last] = task.armed.equal_range(timer);
  task.armed.erase(std::find(first, last, timer));
  timer->is_armed = false;
}

void arm(TimerTask& task, esp_timer* timer, int64_t alarm, int64_t period) {
  timer->alarm = alarm;
  timer->period = period;
  timer->is_armed = true;
  task.armed.insert(timer);
}

void run_timers() {
  TimerTask& task = timer_task();
  std::unique_lock<std::mutex> lock(task.mutex);
  for (;;) {
    if (task.armed.empty()) {
      task.changed.wait(lock);
      continue;
    }
    esp_timer* timer = *task.armed.begin();
    const int64_t now = esp_timer_get_time();
    if (timer->alarm > now) {
      task.changed.wait_for(lock, std::chrono::microseconds(timer->alarm - now));
      continue;
    }
    disarm(task, timer);
    if (timer->period > 0) {
      // Missed periods are made up one after another, unless the timer asked to skip them.
      const bool skip = timer->skip_unhandled_events && (now - timer->alarm) / timer->period > 1;
      arm(task, timer, skip ? now + timer->period : timer->alarm + timer->period, timer->period);
    }
    // Copied, because the callback may delete the timer, and so may any other thread.
    const esp_timer_cb_t callback = timer->callback;
    void* const arg = timer->arg;
    lock.unlock();
    callback(arg);
    lock.lock();
  }
}

esp_err_t start(esp_timer_handle_t timer, int64_t alarm, int64_t period) {
  if (!timer) return ESP_ERR_INVALID_ARG;
  TimerTask& task = timer_task();
  {
    std::lock_guard<std::mutex> lock(task.mutex);
    if (timer->is_armed) return ESP_ERR_INVALID_STATE;
    arm(task, timer, alarm, period);
    if (!task.running) {
      task.running = true;
      std::thread(run_timers).detach();
    }
  }
  task.changed.notify_all();
  return ESP_OK;
}

}  // namespace

extern "C" {

int64_t esp_timer_get_time(void) {
  return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                               start_time())
      .count();
}

esp_err_t esp_timer_create(const esp_timer_create_args_t* args, esp_timer_handle_t* out_handle) {
  if (!args || !args->callback || !out_handle || args->dispatch_method < 0 ||
      args->dispatch_method >= ESP_TIMER_MAX) {
    return ESP_ERR_INVALID_ARG;
  }
  auto* timer = new (std::nothrow) esp_timer{args->callback, args->arg, args->skip_unhandled_events};
  if (!timer) return ESP_ERR_NO_MEM;
  *out_handle = timer;
  return ESP_OK;
}

esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us) {
  return start(timer, esp_timer_get_time() + static_cast<int64_t>(timeout_us), 0);
}

esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us) {
  const auto period = static_cast<int64_t>(std::max(period_us, kMinPeriodUs));
  return start(timer, esp_timer_get_time() + period, period);
}

esp_err_t esp_timer_stop(esp_timer_handle_t timer) {
  if (!timer) return ESP_ERR_INVALID_ARG;
  TimerTask& task = timer_task();
  std::lock_guard<std::mutex> lock(task.mutex);
  if (!timer->is_armed) return ESP_ERR_INVALID_STATE;
  disarm(task, timer);
  return ESP_OK;
}

esp_err_t esp_timer_delete(esp_timer_handle_t timer) {
  if (!timer) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(timer_task().mutex);
  if (timer->is_armed) return ESP_ERR_INVALID_STATE;
  delete timer;
  return ESP_OK;
}

}  // extern "C"
