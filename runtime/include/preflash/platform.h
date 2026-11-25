#pragma once

#include <string>

// What macOS and Linux do differently, behind one name each.
namespace preflash::platform {

// The running executable's full path, or empty if the system won't say.
std::string executable_path();

// The arguments the process started with, as execve takes them.
char** arguments();

// The process's environment, as execve takes it.
char** environment();

// Leaves only the descriptors in keep open across the next exec (the standard three are
// always kept). Returns false if it couldn't. macOS leaves this to posix_spawn's flags, so
// there it does nothing.
bool close_on_exec_except(const int* keep, int count);

}  // namespace preflash::platform
