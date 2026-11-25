#pragma once

#include <string>

#include <nlohmann/json_fwd.hpp>

// The app's end of the link to its window, which is a process of its own (runtime/window) so
// that it stays put while the app restarts. The app writes frames into a shared file and tells
// the window the board and how the device stands. The window sends back touches, button
// presses, turns and a quit.
namespace preflash {
struct Board;
}

namespace preflash::window {

// The window's `board` message: the board, the project, where frames are written, and each
// button's name and wiring in the board file's order.
nlohmann::json board_message(const Board& board, const std::string& project,
                             const std::string& frame_path);

// What the app does with one message from the window: a touch goes to touch::pointer, a
// button to buttons::set, a hello's held buttons replace the buttons the window holds and leave
// a script's holds alone, a rotate steps hardware's rotation a quarter, and a quit ends the
// app. The link's reader thread calls it for each message, and tests can too.
void handle(const nlohmann::json& message);

// Call on the main thread. Shares the panel's frames and connects to the window at
// PREFLASH_WINDOW_SOCKET, starting PREFLASH_WINDOW_EXE if nothing answers. Ends the process
// with a message if no window will stay open.
void open();

// The main thread from here on: opens the sound card when firmware asks, tells the window
// when the device changes, and brings the window back if it goes.
[[noreturn]] void run();

// Tells the window to close, and stops bringing it back. Nothing if there's no window.
// Async-signal-safe.
void close_for_good();

// Whether the terminal app supervises this app, which then leaves its window up on the way
// out for the app it starts next.
bool supervised();

}  // namespace preflash::window
