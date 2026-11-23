#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "preflash/board.h"

// The board's buttons, which the window, the control socket and the command line all press
// through set(). A press moves what the real button moves: a GPIO, an expander input or a
// power key's input. PWR on an AXP2101 and RST only change what's held, for those that act
// on them to hear through on_change.
namespace preflash::buttons {

// Takes the board's buttons, with those named in PREFLASH_HELD (comma-separated) held down,
// as a restart leaves them, and drops names the board lacks. Every pin a button drives rests
// at its released level, or its pressed one if held.
void load(const Board& board);

// Presses or lets go of a button. Empty if it worked, or else why not, naming the board's
// buttons. Pressing a held button, or letting go of one that's up, changes nothing.
std::string set(const std::string& name, bool down);

bool down(const std::string& name);

// The buttons held down, in the order they went down.
std::vector<std::string> held();

// Each button as status shows it, in the board file's order: name, wiring and down.
nlohmann::json describe();

// Called after each change, on the thread that made it.
void on_change(std::function<void(const std::string& name, bool down)> listener);

// What PREFLASH_HELD should say for the next run to come back with these held.
std::string held_env();

}  // namespace preflash::buttons
