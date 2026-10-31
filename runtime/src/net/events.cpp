#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "../os/ticks.h"
#include "esp_event.h"
#include "preflash/fail.h"

// One thread per loop takes events off a queue and runs the matching handlers in the
// order they were posted, which is what firmware relies on.
struct esp_event_handler_instance {
  esp_event_base_t base;
  int32_t id;
  esp_event_handler_t handler;
  void* arg;
};

struct esp_event_loop {
  struct Pending {
    esp_event_base_t base;
    int32_t id;
    std::vector<uint8_t> data;
  };

  size_t capacity = 32;
  std::mutex mutex;
  std::condition_variable changed;
  std::deque<Pending> queue;
  std::vector<std::shared_ptr<esp_event_handler_instance>> handlers;
  bool stopping = false;
  std::thread::id worker_id;
  std::thread worker;

  void run() {
    std::unique_lock<std::mutex> lock(mutex);
    for (;;) {
      changed.wait(lock, [this] { return stopping || !queue.empty(); });
      if (stopping) return;
      Pending event = std::move(queue.front());
      queue.pop_front();
      std::vector<std::shared_ptr<esp_event_handler_instance>> matching;
      for (const auto& h : handlers) {
        const bool base_ok = h->base == nullptr || h->base == event.base;
        const bool id_ok = h->id == ESP_EVENT_ANY_ID || h->id == event.id;
        if (base_ok && id_ok) matching.push_back(h);
      }
      lock.unlock();
      changed.notify_all();  // room in the queue for a poster that was waiting
      for (const auto& h : matching) h->handler(h->arg, event.base, event.id, event.data.data());
      lock.lock();
    }
  }
};

namespace {

// The loops outlive main, so their threads are never joined by a static destructor.
esp_event_loop*& default_loop() {
  static esp_event_loop* loop = nullptr;
  return loop;
}

std::mutex& default_mutex() {
  static std::mutex* m = new std::mutex;
  return *m;
}

esp_event_loop* current_default() {
  std::lock_guard<std::mutex> lock(default_mutex());
  return default_loop();
}

esp_err_t make_loop(size_t capacity, esp_event_loop** out) {
  auto* loop = new esp_event_loop;
  if (capacity > 0) loop->capacity = capacity;
  try {
    loop->worker = std::thread([loop] { loop->run(); });
  } catch (const std::system_error&) {
    delete loop;
    return ESP_ERR_NO_MEM;
  }
  loop->worker_id = loop->worker.get_id();
  *out = loop;
  return ESP_OK;
}

void destroy_loop(esp_event_loop* loop) {
  {
    std::lock_guard<std::mutex> lock(loop->mutex);
    loop->stopping = true;
  }
  loop->changed.notify_all();
  // A handler that deletes its own loop can't wait for itself to finish.
  if (loop->worker.get_id() == std::this_thread::get_id()) {
    loop->worker.detach();
    return;  // the loop object is left for the process to reclaim
  }
  loop->worker.join();
  delete loop;
}

esp_err_t add_handler(esp_event_loop* loop, esp_event_base_t base, int32_t id,
                      esp_event_handler_t handler, void* arg, esp_event_handler_instance** out) {
  if (!loop) return ESP_ERR_INVALID_STATE;
  if (!handler) return ESP_ERR_INVALID_ARG;
  auto instance = std::make_shared<esp_event_handler_instance>(
      esp_event_handler_instance{base, id, handler, arg});
  std::lock_guard<std::mutex> lock(loop->mutex);
  loop->handlers.push_back(instance);
  if (out) *out = instance.get();
  return ESP_OK;
}

template <typename Match>
esp_err_t remove_handler(esp_event_loop* loop, Match match) {
  if (!loop) return ESP_ERR_INVALID_STATE;
  std::lock_guard<std::mutex> lock(loop->mutex);
  for (auto it = loop->handlers.begin(); it != loop->handlers.end(); ++it) {
    if (match(**it)) {
      loop->handlers.erase(it);
      return ESP_OK;
    }
  }
  return ESP_ERR_NOT_FOUND;
}

}  // namespace

extern "C" {

esp_err_t esp_event_loop_create_default(void) {
  std::lock_guard<std::mutex> lock(default_mutex());
  if (default_loop()) return ESP_ERR_INVALID_STATE;
  return make_loop(0, &default_loop());
}

esp_err_t esp_event_loop_delete_default(void) {
  esp_event_loop* loop;
  {
    std::lock_guard<std::mutex> lock(default_mutex());
    loop = default_loop();
    default_loop() = nullptr;
  }
  if (!loop) return ESP_ERR_INVALID_STATE;
  destroy_loop(loop);
  return ESP_OK;
}

esp_err_t esp_event_loop_create(const esp_event_loop_args_t* args, esp_event_loop_handle_t* loop) {
  if (!args || !loop) return ESP_ERR_INVALID_ARG;
  if (!args->task_name) {
    preflash_unsupported("esp_event_loop_create without a task",
                         "a loop that has to be run by hand isn't simulated; give it a task_name");
  }
  return make_loop(args->queue_size > 0 ? static_cast<size_t>(args->queue_size) : 0, loop);
}

esp_err_t esp_event_loop_delete(esp_event_loop_handle_t loop) {
  if (!loop) return ESP_ERR_INVALID_ARG;
  destroy_loop(loop);
  return ESP_OK;
}

esp_err_t esp_event_loop_run(esp_event_loop_handle_t, TickType_t) {
  preflash_unsupported("esp_event_loop_run", "every simulated loop runs on its own thread");
}

esp_err_t esp_event_handler_register(esp_event_base_t base, int32_t id, esp_event_handler_t handler,
                                     void* arg) {
  return add_handler(current_default(), base, id, handler, arg, nullptr);
}

esp_err_t esp_event_handler_register_with(esp_event_loop_handle_t loop, esp_event_base_t base,
                                          int32_t id, esp_event_handler_t handler, void* arg) {
  return add_handler(loop, base, id, handler, arg, nullptr);
}

esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id,
                                              esp_event_handler_t handler, void* arg,
                                              esp_event_handler_instance_t* instance) {
  return add_handler(current_default(), base, id, handler, arg, instance);
}

esp_err_t esp_event_handler_instance_register_with(esp_event_loop_handle_t loop,
                                                   esp_event_base_t base, int32_t id,
                                                   esp_event_handler_t handler, void* arg,
                                                   esp_event_handler_instance_t* instance) {
  return add_handler(loop, base, id, handler, arg, instance);
}

esp_err_t esp_event_handler_unregister(esp_event_base_t base, int32_t id,
                                       esp_event_handler_t handler) {
  return esp_event_handler_unregister_with(current_default(), base, id, handler);
}

esp_err_t esp_event_handler_unregister_with(esp_event_loop_handle_t loop, esp_event_base_t base,
                                            int32_t id, esp_event_handler_t handler) {
  return remove_handler(loop, [&](const esp_event_handler_instance& h) {
    return h.base == base && h.id == id && h.handler == handler;
  });
}

esp_err_t esp_event_handler_instance_unregister(esp_event_base_t base, int32_t id,
                                                esp_event_handler_instance_t instance) {
  return esp_event_handler_instance_unregister_with(current_default(), base, id, instance);
}

esp_err_t esp_event_handler_instance_unregister_with(esp_event_loop_handle_t loop,
                                                     esp_event_base_t base, int32_t id,
                                                     esp_event_handler_instance_t instance) {
  return remove_handler(loop, [&](const esp_event_handler_instance& h) {
    return &h == instance && h.base == base && h.id == id;
  });
}

esp_err_t esp_event_post_to(esp_event_loop_handle_t loop, esp_event_base_t base, int32_t id,
                            const void* data, size_t data_size, TickType_t ticks_to_wait) {
  if (!loop) return ESP_ERR_INVALID_STATE;
  if (!base || (data_size > 0 && !data)) return ESP_ERR_INVALID_ARG;
  std::unique_lock<std::mutex> lock(loop->mutex);
  auto room = [loop] { return loop->stopping || loop->queue.size() < loop->capacity; };
  if (ticks_to_wait == portMAX_DELAY) {
    loop->changed.wait(lock, room);
  } else if (!loop->changed.wait_for(lock, preflash::ticks_to_duration(ticks_to_wait), room)) {
    return ESP_ERR_TIMEOUT;
  }
  if (loop->stopping) return ESP_ERR_INVALID_STATE;
  esp_event_loop::Pending event{base, id, std::vector<uint8_t>(data_size)};
  if (data_size > 0) std::memcpy(event.data.data(), data, data_size);
  loop->queue.push_back(std::move(event));
  lock.unlock();
  loop->changed.notify_all();
  return ESP_OK;
}

esp_err_t esp_event_post(esp_event_base_t base, int32_t id, const void* data, size_t data_size,
                         TickType_t ticks_to_wait) {
  return esp_event_post_to(current_default(), base, id, data, data_size, ticks_to_wait);
}

}  // extern "C"
