#pragma once

#include <cstddef>
#include <functional>
#include <string>

// The virtual serial port: a pseudo-terminal carrying the same stream as the console, for a
// companion app to open as it would a board's USB port. Output goes to the terminal as well
// as everywhere else, and what comes back joins the console's queue: as lines, or as bytes
// just as they came once firmware has installed the USB serial driver, because then "@sim "
// and the ends of lines are the firmware's business.
//
// PREFLASH_PORT asks for it, and names a link to the device. PREFLASH_PORT_LINK names a
// second link. Both ends of the terminal outlive esp_restart, named in PREFLASH_PORT_FDS, so
// a connected app keeps its port. This process holds the far end open too, so that an app
// closing the port leaves its settings as they were: the last close would otherwise reset
// the terminal to echoing input, and firmware would hear its own output.
namespace preflash::port {

// Cuts bytes into the lines the console queue takes. \n, \r and \r\n each end a line.
class LineSplitter {
 public:
  using Emit = std::function<void(const std::string& line)>;

  explicit LineSplitter(size_t longest);

  // Calls emit with each line this finishes. Returns how many lines it threw away for
  // being longer than the limit.
  size_t feed(const char* data, size_t size, const Emit& emit);

  // Hands over a line still waiting for its end.
  std::string take_partial();

 private:
  size_t longest_;
  std::string line_;
  bool after_return_ = false;
  bool too_long_ = false;
};

// Opens the port, or takes over the one a restart left, and starts its threads. Does
// nothing unless PREFLASH_PORT is set. The firmware doesn't need a port, so a failure is a
// message on stderr, and the firmware runs on.
void start();

// Queues output for the terminal. Never waits: past the queue's limit the oldest output
// goes, because with nobody attached the terminal fills and stays full.
void write(const char* data, size_t size);

// Gives what's queued a moment to reach the terminal, and gives up at once if nothing is
// reading it. Async-signal-safe.
void wait_sent();

// Stops taking input, so that what arrives during a restart waits in the terminal for the
// next boot, where a line already taken would be lost.
void hold_input();

// Removes the links, as long as they still point here. A link left behind could end up
// pointing at someone's terminal once the device's name is reused. Async-signal-safe.
void remove_links();

// Closes the terminal, as unplugging a board would, for a process that stays up after a
// crash. Async-signal-safe.
void hang_up();

// The path to give a companion app: the link, or the device if the link failed. Empty with
// no port.
std::string path();
// The pseudo-terminal itself. Empty with no port.
std::string device();

// What a restart must carry over, or -1 with no port.
int master_fd();
int slave_fd();

}  // namespace preflash::port
