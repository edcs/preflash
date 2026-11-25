#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "preflash/board.h"

// The board's power, as the chip sees it. On runs the firmware. Off and download mode run
// none, but keep the window, the control socket and the console answering, and hang up the
// virtual serial port as unplugging the board would. Each change re-runs the process, as
// esp_restart does, with the next mode in PREFLASH_POWER.
namespace preflash::power {

enum class Mode { On, Off, Download };

// Why the chip last came out of reset, as esp_reset_reason tells firmware. PowerCycle is a
// power-on reset too, but the power went and came back, as when the AXP2101 restarts the
// board, so the board's other chips start afresh as well.
enum class Reset { PowerOn, Software, PowerCycle };

// This run's mode, from PREFLASH_POWER: "off", "download", or anything else for on.
Mode mode();

// "on", "off" or "download", as status and the window say it.
const char* name(Mode mode);

// The environment the next run starts with: this one, with PREFLASH_POWER for next,
// PREFLASH_HELD for the buttons held now, PREFLASH_RESET for the reason ("poweron" or "sw"),
// and no PREFLASH_RESTARTED, which older runs set. Each entry is NAME=value.
std::vector<std::string> env_for(Mode next, Reset reason);

// Re-runs the process in the next mode, as esp_restart always has: the socket's clients hear
// it restart, the stream gets a moment to drain, and the virtual serial port goes along. A
// second call while the first is on its way waits for it.
[[noreturn]] void enter(Mode next, Reset reason);

// Where a release of the named button takes the board: a release of RST resets the chip into
// download mode with BOOT (the button on GPIO0, the strapping pin) held down, or else boots it.
// Nothing for any other button, or while the board is off and RST has no power to act on.
std::optional<Mode> after_release(const Board& board, const std::string& released);

// Has each release of the board's RST enter the mode after_release names. Call once.
void watch_reset(const Board& board);

// How often a board with a power key looks at its circuit, and the AXP2101 at its PWRON.
constexpr int kPowerPollMs = 10;

// Where a power key's circuit leaves the board: on while USB feeds it, and on battery while
// the key is down or the hold is high.
Mode decide(bool usb, bool key, bool hold);

// Whether the firmware holds the board on: its power key's hold is an output driven high.
// False on a board with no power key.
bool holding(const Board& board);

// Whether this launch starts off: a board with a power key, launched on battery with the key
// up. A re-run, which PREFLASH_POWER marks, isn't a launch.
bool starts_off(const Board& board);

// Goes to the next mode through enter, or through the test's stand-in if one is set.
void switch_to(Mode next, Reset reason);

// The same on a thread of its own, so that the caller holds nothing enter needs and can still
// answer whoever asked.
void switch_soon(Mode next, Reset reason);

// Has switch_to call this in place of enter, which a test can't live through. Null puts enter
// back.
void set_enter_for_test(std::function<void(Mode, Reset)> stand_in);

// Hears the board's power buttons, for this run's mode. Off, a press of PWR boots the board.
// On, the AXP2101 hears its PWRON. Call once, after buttons::load and before anything can
// press a button, such as the window's hello, so that no press goes unheard.
void listen_power(const Board& board);

// Watches a power key's circuit, for this run's mode. Off, USB coming back boots the board.
// On, the board goes off when decide says so. Call once, after buttons::load.
void watch_power(const Board& board);

}  // namespace preflash::power

namespace preflash {

// Replaces this process with a fresh run of path, with envp for its environment, keeping only
// the standard three descriptors and the virtual serial port, with every signal at its default
// but those main must find ignored. Returns only on failure. enter's way out.
int exec_clean(const char* path, char* const argv[], char* const envp[]);

}  // namespace preflash
