// A firmware that crashes as it's told to, since a crash can't happen in the suite's process.
#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "esp_err.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "preflash/crash.h"
#include "preflash/fail.h"
#include "preflash/panel.h"
#include "preflash/serial.h"

namespace {

std::string g_mode;
std::atomic<long> g_counted{0};

__attribute__((noinline)) int read_null(volatile int* where) { return *where; }

__attribute__((noinline)) void write_null(volatile int* where) { *where = 1; }

__attribute__((noinline)) int use_the_stack(int depth) {
  volatile char room[4096];
  room[0] = static_cast<char>(depth);
  return use_the_stack(depth + 1) + room[0];
}

__attribute__((noinline)) void crash_now() {
  if (g_mode == "read") read_null(nullptr);
  if (g_mode == "write") write_null(nullptr);
  if (g_mode == "abort") std::abort();
  if (g_mode == "unsupported") preflash_unsupported("flash_op", "because");
  if (g_mode == "check") ESP_ERROR_CHECK(ESP_ERR_NO_MEM);
  if (g_mode == "system_abort") esp_system_abort("heap corrupt");
  if (g_mode == "stack") use_the_stack(0);
  if (g_mode == "locked") {
    flockfile(stdout);
    read_null(nullptr);
  }
  if (g_mode == "draw") {
    const int fault_rows = 10;
    preflash::panel::draw(0, 0, 10, fault_rows, nullptr, true);
  }
  if (g_mode == "quote") preflash_unsupported("say\"\\\n", "tab\there");
  if (g_mode == "illegal") __builtin_trap();
}

void crashing_task(void*) {
  vTaskDelay(pdMS_TO_TICKS(50));
  crash_now();
  vTaskDelete(nullptr);
}

void counting_task(void*) {
  for (;;) {
    ++g_counted;
    vTaskDelay(1);
  }
}

// A held app's window and socket go on, so the helper's main thread plays them.
void wait_and_report_hold(const char* path) {
  while (!preflash::crash::happened()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const long before = g_counted;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  const long after = g_counted;
  const auto frame = preflash::panel::copy_frame();
  std::ofstream(path) << "counted " << after - before << " frame " << frame.size() << "\n";
  _exit(7);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) return 2;
  g_mode = argv[1];
  if (!preflash::serial::start()) return 3;
  xTaskCreate(counting_task, "counter", 4096, nullptr, 1, nullptr);
  xTaskCreate(crashing_task, "painter", 4096, nullptr, 1, nullptr);
  if (argc > 2) wait_and_report_hold(argv[2]);
  for (;;) pause();
}
