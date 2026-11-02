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

// Tells every client, ahead of whatever is queued for it, that this process is about to
// restart and to reconnect. New connections are turned away from here on.
void announce_restart();

// Ends the process, with the socket removed. The stream gets a bounded wait to drain, so
// output can still be lost to a stuck client or terminal. Goes out by _exit, because
// firmware threads may hold locks that destructors would need. Async-signal-safe.
[[noreturn]] void leave(int code);

// leave(0), first flushing stdio, except a stream another thread has locked, which could
// be for good and so is skipped.
[[noreturn]] void quit();

}  // namespace preflash::socket
