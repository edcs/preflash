#pragma once

#include <string>

// The control socket: newline-delimited JSON over the Unix socket that PREFLASH_SOCKET
// names. The TUI, the command line, CI and agents all come through it.
namespace preflash::socket {

// Takes the socket, clearing a stale one left by an app that's gone. False if another
// live app holds it, with error saying which. True and nothing done if the variable is
// unset. Clients can queue from here on, and serve() starts answering them.
bool claim(std::string* error);

// Starts answering clients. False if it couldn't start.
bool serve();

// Tells every client that this process is about to restart and to reconnect, after
// which new connections are turned away.
void announce_restart();

// Ends the process, with the stream drained and the socket removed. Goes out by _exit,
// because firmware threads may hold locks that destructors would need. Async-signal-safe.
[[noreturn]] void leave(int code);

// leave(0), first writing out what stdio holds.
[[noreturn]] void quit();

}  // namespace preflash::socket
