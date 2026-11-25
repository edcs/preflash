#include <signal.h>
#include <unistd.h>

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <optional>
#include <string>
#include <vector>

#include "esp_timer.h"
#include "freertos/task.h"
#include "preflash/audio.h"
#include "preflash/board.h"
#include "preflash/buttons.h"
#include "preflash/chips.h"
#include "preflash/control.h"
#include "preflash/hardware.h"
#include "preflash/panel.h"
#include "preflash/port.h"
#include "preflash/power.h"
#include "preflash/serial.h"
#include "preflash/socket.h"
#include "preflash/touch.h"
#include "preflash/trace.h"
#include "window.h"

extern "C" void app_main(void);

namespace {

// Stack size as ESP-IDF's main task has it, though the host ignores it.
constexpr uint32_t kMainTaskStack = 3584;
constexpr UBaseType_t kMainTaskPriority = 1;

void on_interrupt(int) { preflash::socket::leave(0); }

void main_task(void*) { app_main(); }

std::optional<int> parse_number(const std::string& word) {
  int value;
  const char* end = word.data() + word.size();
  const auto result = std::from_chars(word.data(), end, value);
  if (result.ec != std::errc() || result.ptr != end) return std::nullopt;
  return value;
}

// A press straight into the touch queue, from the console task.
void control_touch(const std::vector<std::string>& args) {
  const auto x = args.size() >= 2 ? parse_number(args[0]) : std::nullopt;
  const auto y = args.size() >= 2 ? parse_number(args[1]) : std::nullopt;
  const auto ms = args.size() >= 3 ? parse_number(args[2]) : preflash::touch::kDefaultPressMs;
  if (!x || !y || !ms || !preflash::touch::press(*x, *y, *ms)) {
    std::printf("@sim touch needs x y [ms], with x and y on the panel\n");
  }
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
  // Starts the clock, so that uptime counts from here and not from the first caller.
  esp_timer_get_time();
  // Line by line, so that a line reaches the stream as firmware finishes it.
  setvbuf(stdout, nullptr, _IOLBF, 0);
  // Before SDL starts, so that SDL leaves them in place. SIGINT can't honour an inherited
  // ignore, because a shell's background job starts with one, but nohup's must stand.
  signal(SIGINT, on_interrupt);
  for (const int stop : {SIGTERM, SIGHUP}) {
    if (signal(stop, on_interrupt) == SIG_IGN) signal(stop, SIG_IGN);
  }
  // The window process is this one's child, and nothing waits for it: when it ends, it goes
  // without a zombie left behind.
  signal(SIGCHLD, SIG_IGN);

  // Off and download mode run no firmware, but answer as the board would: the window, the
  // socket and the console stay, and the serial port hangs up.
  preflash::power::Mode mode = preflash::power::Mode::On;
  try {
    // Before the chips, which read it as they start.
    preflash::hardware::load(preflash::board());
    // Before firmware reads a pin, so that a button held through a restart is down at boot.
    preflash::buttons::load(preflash::board());
    // A board with a power key, launched on battery, waits for PWR. Said in the environment,
    // where everything that asks for the mode looks.
    if (preflash::power::starts_off(preflash::board())) setenv("PREFLASH_POWER", "off", 1);
    mode = preflash::power::mode();
    if (mode == preflash::power::Mode::On) preflash::attach_chips(preflash::board());
  } catch (const std::exception& e) {
    std::fprintf(stderr, "preflash: %s\n", e.what());
    return 1;
  }
  const bool running = mode == preflash::power::Mode::On;
  preflash::power::watch_reset(preflash::board());

  const char* headless_env = std::getenv("PREFLASH_HEADLESS");
  const bool headless = headless_env && std::strcmp(headless_env, "1") == 0;
  // Before the window, so that a second app stops without taking over the first one's.
  std::string socket_error;
  if (!preflash::socket::claim(&socket_error)) {
    std::fprintf(stderr, "preflash: %s\n", socket_error.c_str());
    return 1;
  }

  if (!headless) preflash::window::open();

  // After the window, whose failures print straight to the terminal.
  if (!preflash::serial::start()) preflash::socket::leave(1);
  const std::string forced = running ? preflash::hardware::forced_summary() : "";
  if (!forced.empty()) {
    preflash::serial::notice("preflash: forced on this boot: " + forced + "\n");
  }
  // Tracing doesn't stop for a restart, so a client can see the boot it comes back to.
  if (std::getenv("PREFLASH_TRACE")) preflash::trace::start_early();
  preflash::add_control("touch", control_touch);
  preflash::add_control("audio", control_audio);
  preflash::add_control("brightness", control_brightness);
  preflash::add_control("shot", control_shot);
  preflash::add_control("quit", [](std::vector<std::string>) { preflash::socket::quit(); });
  // After the stream, so that the toolbar's reader hears any trouble with it, and after the
  // controls, because a restart can leave a quit waiting in the terminal.
  if (running) {
    preflash::port::start();
  } else {
    preflash::port::let_go();
  }
  if (!preflash::start_console_reader()) {
    std::fprintf(stderr, "preflash: couldn't start the console task\n");
    preflash::socket::leave(1);
  }

  if (!preflash::socket::serve()) {
    std::fprintf(stderr, "preflash: couldn't start answering the control socket\n");
    preflash::socket::leave(1);
  }

  // Once the socket and the stream are up for enter to tell, and before the firmware starts.
  preflash::power::watch_power(preflash::board());

  if (mode == preflash::power::Mode::Download) {
    // What the ROM says when GPIO0 is low as the chip comes out of an EN reset.
    std::printf("rst:0x1 (POWERON),boot:0x0 (DOWNLOAD(USB/UART0))\nwaiting for download\n");
    std::fflush(stdout);
  } else if (running && xTaskCreate(main_task, "main", kMainTaskStack, nullptr, kMainTaskPriority,
                                    nullptr) != pdPASS) {
    std::fprintf(stderr, "preflash: couldn't start the main task\n");
    preflash::socket::leave(1);
  }

  if (!headless) preflash::window::run();
  // The firmware runs on after app_main returns, as on a device.
  for (;;) pause();
}
