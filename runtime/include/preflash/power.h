#pragma once

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

// Why the chip last came out of reset, as esp_reset_reason tells firmware.
enum class Reset { PowerOn, Software };

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

}  // namespace preflash::power

namespace preflash {

// Replaces this process with a fresh run of path, with envp for its environment, keeping only
// the standard three descriptors and the virtual serial port, with every signal at its default
// but those main must find ignored. Returns only on failure. enter's way out.
int exec_clean(const char* path, char* const argv[], char* const envp[]);

}  // namespace preflash
