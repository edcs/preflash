#pragma once

#include <functional>
#include <string>
#include <vector>

namespace preflash {

// Registers a control for lines of the form "@sim <name> args...". The handler
// gets the words after the name. A later registration of a name replaces the earlier one.
// Controls run on the console task's thread, so anything touching the window
// must hand off to the main thread.
void add_control(const char* name, std::function<void(std::vector<std::string>)> fn);

// Runs one console line. "@sim " lines go to the control named by the first word
// after it; anything else goes to the console command of that name. A name
// nobody registered prints a message saying so.
void dispatch_line(const std::string& line);

}  // namespace preflash
