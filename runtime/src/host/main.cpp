#include <signal.h>
#include <unistd.h>

#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "freertos/task.h"
#include "preflash/audio.h"
#include "preflash/board.h"
#include "preflash/chips.h"
#include "preflash/control.h"
#include "preflash/panel.h"
#include "preflash/touch.h"
#include "window.h"

extern "C" void app_main(void);

namespace {

constexpr int kDefaultPressMs = 80;

// Stack size as ESP-IDF's main task has it, though the host ignores it.
constexpr uint32_t kMainTaskStack = 3584;
constexpr UBaseType_t kMainTaskPriority = 1;

// Not exit(): firmware threads may be holding locks that destructors would need.
void quit(int) { _exit(0); }

void main_task(void*) { app_main(); }

std::optional<int> parse_number(const std::string& word) {
  int value;
  const char* end = word.data() + word.size();
  const auto result = std::from_chars(word.data(), end, value);
  if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
  return value;
}

// A press straight into the touch queue, because this runs on the console task and
// only the main thread may touch SDL.
void control_touch(const std::vector<std::string>& args) {
  const auto x = args.size() >= 2 ? parse_number(args[0]) : std::nullopt;
  const auto y = args.size() >= 2 ? parse_number(args[1]) : std::nullopt;
  const auto ms = args.size() >= 3 ? parse_number(args[2]) : kDefaultPressMs;
  const preflash::Board& board = preflash::board();
  if (!x || !y || !ms || *ms < 0 || *x < 0 || *y < 0 || *x >= board.width || *y >= board.height) {
    std::printf("@sim touch needs x y [ms], with x and y on the panel\n");
    return;
  }
  preflash::touch::push({*x, *y, true});
  std::this_thread::sleep_for(std::chrono::milliseconds(*ms));
  preflash::touch::push({*x, *y, false});
}

void control_shot(const std::vector<std::string>& args) {
  if (args.size() != 1) {
    std::printf("@sim shot needs a file name\n");
    return;
  }
  try {
    preflash::panel::save_bmp(args[0].c_str());
    std::printf("saved %s\n", args[0].c_str());
  } catch (const std::exception& e) {
    std::printf("@sim shot failed: %s\n", e.what());
  }
}

void control_audio(const std::vector<std::string>&) {
  std::printf("audio samples written: %zu\n", preflash::audio_samples_written());
}

void control_brightness(const std::vector<std::string>&) {
  std::printf("%s\n", preflash::panel::backlight_status().c_str());
}

}  // namespace

int main() {
  // A piped stdout must keep up, because the process leaves through _exit.
  setvbuf(stdout, nullptr, _IOLBF, 0);
  // Before SDL starts, so that SDL leaves it in place.
  signal(SIGINT, quit);

  try {
    preflash::attach_chips(preflash::board());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "preflash: %s\n", e.what());
    return 1;
  }

  const char* headless_env = std::getenv("PREFLASH_HEADLESS");
  const bool headless = headless_env && std::strcmp(headless_env, "1") == 0;
  if (!headless) preflash::window::open();

  preflash::add_control("touch", control_touch);
  preflash::add_control("audio", control_audio);
  preflash::add_control("brightness", control_brightness);
  preflash::add_control("shot", control_shot);
  preflash::add_control("quit", [](std::vector<std::string>) { quit(0); });
  if (!preflash::start_console_reader()) {
    std::fprintf(stderr, "preflash: couldn't start the console task\n");
    return 1;
  }

  if (xTaskCreate(main_task, "main", kMainTaskStack, nullptr, kMainTaskPriority, nullptr) != pdPASS) {
    std::fprintf(stderr, "preflash: couldn't start the main task\n");
    return 1;
  }

  if (!headless) preflash::window::run();
  // The firmware runs on after app_main returns, as on a device.
  for (;;) pause();
}
