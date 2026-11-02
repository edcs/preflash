#pragma once

// What happens when firmware dies. Handlers for the fatal signals write a report to
// PREFLASH_CRASH_FILE, tell the serial stream and drain it. The process then dies of its
// signal, or with PREFLASH_CRASH_HOLD set stops in place: firmware's threads stay frozen
// where they were, while the window and the socket go on, until something ends the process.
namespace preflash::crash {

// Reads the environment and takes over the fatal signals. Call it once.
void install();

// Makes this thread one of firmware's: a crash names it and freezes it, and it gets an
// alternate stack, so that overflowing its own can still be reported.
void enter_task(const char* name);

// Undoes that, as a thread finishes.
void leave_task();

// Says why this thread is about to abort, for the report. It copies both strings.
void note(const char* kind, const char* message);

// Whether a crash has been caught. Once it's true, a lock firmware could have held may
// never come free, so anything that must go on stops taking them.
bool happened();

}  // namespace preflash::crash
