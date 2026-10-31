#pragma once

// The window onto the virtual panel. The only code that makes SDL video calls.
namespace preflash::window {

// Call on the main thread. Exits with a message if there can be no window.
void open();

// Runs the window on the main thread until it's closed, which ends the process.
[[noreturn]] void run();

}  // namespace preflash::window
