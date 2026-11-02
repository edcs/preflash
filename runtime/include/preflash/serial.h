#pragma once

#include <cstddef>
#include <deque>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

// USB serial is one stream, as on the device. Once started, stdout and stderr both
// write to a pipe, and a pump thread passes what comes out to a ring of recent output,
// each subscriber, the virtual serial port and the real stdout. None of them can hold the
// pump up: each has a bounded queue and a thread of its own.
namespace preflash::serial {

std::string base64(const void* data, size_t size);

// The newest bytes, up to a fixed size. Not locked: the pump guards it.
class Ring {
 public:
  explicit Ring(size_t capacity);
  void append(const char* data, size_t size);
  std::string contents() const;

 private:
  std::vector<char> bytes_;
  size_t next_ = 0;
  bool full_ = false;
};

// What's waiting to go to one socket client, as lines of the wire format. Several threads
// fill it (replies, the pump, restart) and the mutex keeps them apart, while one thread
// writes it out, so a client that stops reading costs firmware nothing: past the limit,
// the oldest serial goes, and the client hears how much it missed.
class Outbox {
 public:
  explicit Outbox(size_t serial_limit);
  ~Outbox();

  // A reply or an event, without its newline. These are never dropped.
  void push_message(std::string line);
  void push_serial(const char* data, size_t size);
  // A trace record, with a limit of its own the same size as serial's. Past that the oldest
  // goes, which a client sees as a gap in the records' numbers.
  void push_trace(std::string line);
  // A message that goes ahead of everything queued, for news that can't wait behind a
  // slow client's backlog.
  void push_urgent(std::string line);

  // Waits for the next line. False once closed with nothing left. Call sent() after
  // writing the line.
  bool pop(std::string* line);
  void sent();

  // No more lines come, and what's queued can still be taken.
  void close();
  // The client is gone, so what's queued goes too.
  void abandon();

  // Bytes of replies and events waiting.
  size_t message_bytes();

 private:
  enum class Kind { Message, Serial, Trace };
  struct Item {
    std::string bytes;
    Kind kind;
  };
  void forget(long items);

  std::mutex mutex_;
  std::condition_variable ready_;
  std::deque<Item> items_;
  size_t serial_limit_;
  size_t serial_bytes_ = 0;
  size_t trace_bytes_ = 0;
  size_t message_bytes_ = 0;
  size_t dropped_ = 0;
  long unsent_ = 0;
  long in_flight_ = 0;  // popped, and sent() not yet called
  bool closed_ = false;
};

// Lines waiting in every Outbox and chunks waiting for the real stdout, which leaving the
// process gives a moment to clear.
long unsent();

// Swaps fds 1 and 2 for the pipe and starts the pump. False if it couldn't, with the
// reason on stderr. Also stops a crash losing what's still in the pipe.
bool start();

// Called with the ring's contents, then with everything after them, in order. The
// callback runs on the pump, so it mustn't block. Returns a number for unsubscribe.
using Subscriber = std::function<void(const char* data, size_t size)>;
int subscribe(Subscriber subscriber);
void unsubscribe(int subscription);

// Tells the people watching something firmware didn't say: it reaches the ring, subscribers
// and the real stdout, and stays off the virtual serial port, where a companion app would
// read it as firmware's. Never waits.
void notice(const std::string& text);

std::string ring_contents();

// Blocks the signals that belong to firmware's threads. For the threads that serve the
// stream, which must never run a signal handler that waits for them.
void mask_signals_on_this_thread();

// Writes out what stdio holds, unless another thread has it locked, which could be for
// good. Separate from hand_back because stdio can't be used in a signal handler.
void flush_stdio();

// Puts the real fds 1 and 2 back, so anything written from here on goes straight out,
// then gives the pump a moment to finish. Async-signal-safe, safe to call twice,
// and does nothing if the pump never started.
void hand_back();

// Gives subscribers' writers a moment to clear what's waiting. Async-signal-safe. The
// wait is bounded, because a client that stopped reading never will clear it.
void wait_sent();

// Both of the above, and the virtual serial port's links gone, for a process about to end.
void release();

}  // namespace preflash::serial
