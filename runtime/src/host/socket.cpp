#include "preflash/socket.h"

#include <unistd.h>

#include "preflash/serial.h"

namespace preflash::socket {

void announce_restart() {}

void leave(int code) {
  serial::release();
  _exit(code);
}

void quit() {
  serial::flush_stdio();
  leave(0);
}

}  // namespace preflash::socket
