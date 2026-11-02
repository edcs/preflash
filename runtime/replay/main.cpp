// Replays transactions against the chip models of the board named by PREFLASH_BOARD, for
// preflash check. It has no firmware, no window and no socket.
#include <exception>
#include <iostream>

#include "preflash/board.h"
#include "preflash/chips.h"
#include "preflash/hardware.h"
#include "preflash/replay.h"

int main() {
  try {
    preflash::hardware::load(preflash::board());
    preflash::attach_chips(preflash::board());
    preflash::replay(std::cin, std::cout);
  } catch (const std::exception& e) {
    std::cerr << e.what() << "\n";
    return 1;
  }
  return 0;
}
