#pragma once

#include <cstdint>
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

// Who holds a button: the device window's mouse, or a script through the socket or the
// command line. A button is down while either holds it.
enum class Owner { Window, Script };

// Takes the board's buttons, with those PREFLASH_HELD names held down by their owners, as a
// restart leaves them, and drops names the board lacks. A reset button comes back up: the
// reset that started this run took its press, as a chip can't run with EN held. PREFLASH_HELD
// is what held_env() wrote: comma-separated name:owner pairs, where a bare name is a script's.
// A pair with @deadline is let go at that CLOCK_REALTIME ms, or straight away once it's past.
// Every pin a button drives rests at its released level, or its pressed one if held.
void load(const Board& board);

// Presses or lets go of a button for one owner. The button goes down with its first owner and
// up with its last, and only those edges move its pin, trace and tell listeners. Empty if it
// worked, or else why not, naming the board's buttons.
std::string set(const std::string& name, bool down, Owner owner = Owner::Script);

// Whether anyone holds the button.
bool down(const std::string& name);

// The buttons held down by anyone, in the order they went down.
std::vector<std::string> held();

// The buttons this owner holds, in the order it pressed them.
std::vector<std::string> held_by(Owner owner);

// Each button as status shows it, in the board file's order: name, wiring and down.
nlohmann::json describe();

// A press that lets go by itself at release_at, a CLOCK_REALTIME time in ms, if nothing lets
// go of it first. The release is the caller's to make: the deadline is for the next run, when
// an exec cuts the caller off, and travels in held_env().
std::string press_until(const std::string& name, Owner owner, int64_t release_at);

// Presses the button, holds it for ms and lets go, returning once it has. A tap that
// cut_taps wakes returns at once, empty and still down: the exec under way leaves the release
// to the next run, at the same moment.
std::string tap(const std::string& name, int ms, Owner owner = Owner::Script);

// Wakes every tap under way, and any that starts after, without letting go: the run is ending,
// and held_env() has already said when each lets go. power::enter calls it once it has built
// the next run's environment, so that a tap's caller can still answer. load() puts it back.
void cut_taps();

// Called after each change, on the thread that made it.
void on_change(std::function<void(const std::string& name, bool down)> listener);

// What PREFLASH_HELD should say for the next run to come back with these held by the same
// owners, such as "BOOT:window,PWR:script". A press with a deadline adds it after an @, as
// "PWR:script@1762300000000".
std::string held_env();

}  // namespace preflash::buttons
