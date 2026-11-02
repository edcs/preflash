#include "console.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "audio.h"
#include "board.h"
#include "esp_console.h"
#include "esp_system.h"
#include "mishaps.h"
#include "monitor.h"
#include "network.h"
#include "settings.h"
#include "timekeeper.h"
#include "ui/ui.h"
#include "update.h"

namespace console {
namespace {

constexpr int kSpewLine = 64;

// A percentage from the command line, or -1 if it isn't one.
int percent_from(const char* text) {
  char* end = nullptr;
  const long value = std::strtol(text, &end, 10);
  return *end == '\0' && value >= 0 && value <= 100 ? static_cast<int>(value) : -1;
}

int fail(const char* message) {
  std::printf("%s\n", message);
  return 1;
}

int status_command(int, char**) {
  const monitor::Snapshot chips = monitor::latest();
  const network::Status net = network::status();
  const update::Status slots = update::status();
  const settings::Values saved = settings::get();
  std::printf("board=%s boots=%lu brightness=%d volume=%d\n", board::name(), static_cast<unsigned long>(saved.boots),
              saved.brightness, saved.volume);
  std::printf("rtc=%s time=%lld held=%d\n", ui::describe(chips.rtc), static_cast<long long>(timekeeper::now()),
              timekeeper::held());
  std::printf("motion=%s x=%d y=%d turned=%d\n", chips.motion.answered ? "ok" : "no answer", chips.motion.x_mg,
              chips.motion.y_mg, chips.motion.degrees);
  if (board::has_power_chip()) {
    std::printf("power=%s usb=%d percent=%d\n", chips.power.answered ? "ok" : "no answer", chips.power.usb,
                chips.power.percent);
  }
  std::printf("network=%s ssid=%s address=%s\n", ui::describe(net.wifi), net.ssid, net.address);
  std::printf("fetch=%s http=%d bytes=%d error=%s\n", ui::describe(net.fetch), net.http_status, net.bytes,
              esp_err_to_name(net.fetch_error));
  std::printf("update=%s slot=%s version=%s\n", ui::describe(slots.check), slots.running_slot, slots.version);
  std::printf("sound=%s frames=%u\n", audio::available() ? "yes" : "no", audio::frames_played());
  return 0;
}

int page_command(int argc, char** argv) {
  if (argc != 2 || !ui::open_by_name(argv[1])) return fail("usage: page home|time|motion|power|sound|network|update|settings|faults");
  return 0;
}

int brightness_command(int argc, char** argv) {
  const int percent = argc == 2 ? percent_from(argv[1]) : -1;
  if (percent < 0) return fail("usage: brightness 0-100");
  settings::set_brightness(percent);
  board::set_brightness(percent);
  std::printf("brightness=%d\n", percent);
  return 0;
}

int volume_command(int argc, char** argv) {
  const int percent = argc == 2 ? percent_from(argv[1]) : -1;
  if (percent < 0) return fail("usage: volume 0-100");
  settings::set_volume(percent);
  audio::set_volume(percent);
  std::printf("volume=%d\n", percent);
  return 0;
}

int reset_command(int, char**) {
  settings::reset();
  const settings::Values values = settings::get();
  board::set_brightness(values.brightness);
  audio::set_volume(values.volume);
  std::printf("settings back to defaults\n");
  return 0;
}

int beep_command(int, char**) {
  if (!audio::available()) return fail("this board plays no sound");
  audio::beep();
  std::printf("beep\n");
  return 0;
}

int time_command(int argc, char** argv) {
  time_t utc;
  if (argc != 2 || !timekeeper::parse(argv[1], &utc)) return fail("usage: time 2026-07-14T09:41:00Z");
  timekeeper::set(utc, timekeeper::Source::console);
  std::printf("time set\n");
  return 0;
}

int freeze_command(int argc, char** argv) {
  if (argc == 2 && std::strcmp(argv[1], "off") == 0) {
    timekeeper::release();
    std::printf("clock=running\n");
    return 0;
  }
  time_t utc;
  if (argc != 2 || !timekeeper::parse(argv[1], &utc)) return fail("usage: freeze 2026-07-14T09:41:00Z|off");
  timekeeper::hold(utc);
  std::printf("clock=held\n");
  return 0;
}

int wifi_command(int argc, char** argv) {
  if (argc < 2 || argc > 3) return fail("usage: wifi ssid [password]");
  const char* password = argc == 3 ? argv[2] : "";
  settings::set_wifi(argv[1], password);
  network::join(argv[1], password);
  std::printf("joining %s\n", argv[1]);
  return 0;
}

int fetch_command(int, char**) {
  network::fetch();
  std::printf("fetching\n");
  return 0;
}

int update_command(int, char**) {
  update::check();
  std::printf("checking\n");
  return 0;
}

int restart_command(int, char**) {
  std::printf("restarting\n");
  esp_restart();
}

int poweroff_command(int, char**) {
  if (!board::has_power_chip()) return fail("this board has no power chip");
  power::turn_off();
  return 0;
}

int crash_command(int argc, char** argv) {
  const char* how = argc == 2 ? argv[1] : "null";
  if (std::strcmp(how, "null") == 0) mishaps::null_read();
  if (std::strcmp(how, "abort") == 0) mishaps::abort_now();
  if (std::strcmp(how, "check") == 0) mishaps::failed_check();
  return fail("usage: crash null|abort|check");
}

int hang_command(int, char**) {
  monitor::hang();
  std::printf("waiting for the watchdog\n");
  return 0;
}

// Prints this many bytes as fast as the console takes them, to measure the serial stream.
int spew_command(int argc, char** argv) {
  if (argc != 2) return fail("usage: spew bytes");
  char line[kSpewLine];
  std::memset(line, 'x', kSpewLine - 1);
  line[kSpewLine - 1] = '\n';
  for (long left = std::strtol(argv[1], nullptr, 10); left > 0; left -= kSpewLine) {
    std::fwrite(line, 1, std::min<long>(left, kSpewLine), stdout);
  }
  std::printf("spew done\n");
  return 0;
}

void add(const char* name, const char* help, esp_console_cmd_func_t func) {
  esp_console_cmd_t command = {};
  command.command = name;
  command.help = help;
  command.func = func;
  ESP_ERROR_CHECK(esp_console_cmd_register(&command));
}

}  // namespace

void start() {
  esp_console_repl_t* repl = nullptr;
  esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
  repl_config.prompt = "demo>";
  esp_console_dev_usb_serial_jtag_config_t device = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&device, &repl_config, &repl));

  ESP_ERROR_CHECK(esp_console_register_help_command());
  add("status", "Prints what the firmware knows", status_command);
  add("page", "Opens a page", page_command);
  add("brightness", "Sets and saves the backlight", brightness_command);
  add("volume", "Sets and saves the volume", volume_command);
  add("reset", "Puts the settings back", reset_command);
  add("beep", "Plays a tone", beep_command);
  add("time", "Sets the RTC", time_command);
  add("freeze", "Holds the clock and stops anything moving", freeze_command);
  add("wifi", "Joins a network and saves it", wifi_command);
  add("fetch", "Fetches the check page over HTTPS", fetch_command);
  add("update", "Checks for a newer version", update_command);
  add("restart", "Restarts the app", restart_command);
  add("poweroff", "Turns the board off through its power chip", poweroff_command);
  add("crash", "Crashes on purpose: null, abort or check", crash_command);
  add("hang", "Stops feeding the watchdog", hang_command);
  add("spew", "Prints many bytes, to measure the serial stream", spew_command);

  ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

}  // namespace console
