#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>

#include "preflash/board.h"

// The board as someone holding it could change it: which way up it is, its power, and the
// failures forced on it. Each item applies straight away, or at the next boot if a running
// board couldn't lose it safely. <state>/faults.json holds them all, so they outlast a
// restart and a rebuild until they're set back. Chip models read these as firmware
// reaches them, so firmware sees a change through its usual calls.
namespace preflash::hardware {

// Takes the state in the file as this boot's, leaving out what the board can't take.
void load(const Board& board);

// What's forced at this boot and differs from a healthy board, in words, or empty.
std::string forced_summary();

// Every item: name, label, group, applies ("now" or "boot"), value (in force now), next (what
// the next boot gets) and options (value and label).
nlohmann::json describe();

// Changes an item. Empty if it worked, or else why not.
std::string set(const std::string& name, const std::string& value);

int rotation();          // degrees clockwise from upright
bool usb_in();           // USB power is there
int battery_percent();   // -1 with none fitted
bool rtc_flat();         // the RTC's cell was flat at this boot
bool i2c_nacks(int port, uint8_t address);  // a forced failure answers for this chip

// Ends the process as a power chip does a board, ready for USB to come back next time.
[[noreturn]] void switch_off();

}  // namespace preflash::hardware
