#pragma once

#include <functional>
#include <string>
#include <vector>

namespace preflash {

// Registers a control for lines of the form "@sim <name> args...". The handler
// gets the words after the name. A later registration of a name replaces the earlier one.
// Controls run on the console task, or on the calling thread for "@sim quit", so
// anything touching the window must hand off to the main thread.
void add_control(const char* name, std::function<void(std::vector<std::string>)> fn);

// Runs one console line. "@sim " lines go to the control named by the first word
// after it; anything else goes to the console command of that name. A name
// nobody registered prints a message saying so. The queue doesn't use this for a line
// that belongs to firmware's USB serial driver.
void dispatch_line(const std::string& line);

// Adds a line to the one queue that stdin and the control socket share. Lines run in
// order on the console task, and a firmware command waits there until a REPL opens,
// with every later line behind it. "@sim quit" stays out of the queue and runs on the
// calling thread, after finish_queued_lines, so a stuck command can't stop it. False, with
// the line dropped, if the queue is full, as a device's UART buffer would overflow.
bool submit_line(const std::string& line);

// Adds bytes for firmware's USB serial driver to the same queue, in order behind any lines.
// Nothing in them means anything to the simulator, so "@sim " isn't looked for. Use it
// once firmware_reads_serial is true. False, with the bytes dropped, if the queue is full.
bool submit_bytes(const char* data, size_t size);

// True once firmware has installed the USB serial driver. From then on its lines go to
// usb_serial_jtag_read_bytes, in place of a REPL, with a newline after each.
bool firmware_reads_serial();

// True for a line that submit_line runs as "@sim quit", which doesn't come back.
bool is_quit_line(const std::string& line);

// Waits for the lines queued now, so that quitting doesn't lose them. Gives up once a line
// has run for a second without finishing, or one is waiting for a REPL.
void finish_queued_lines();

// Starts the console task and the reader that feeds it from stdin until stdin ends. Safe to
// call more than once, and a failed start can be tried again. False if either couldn't start.
bool start_console_reader();

// True once the reader has stopped reading stdin. Tests use it to know the
// task no longer touches the input they are about to replace.
bool console_reader_done();

}  // namespace preflash
