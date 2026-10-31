#pragma once

#include <optional>

// Touches in the order they happened, so that a press and release that both
// arrive between two reads still reach firmware as two events.
namespace preflash::touch {

struct Event {
  int x, y;
  bool down;
};

void push(Event e);
std::optional<Event> pop();

}  // namespace preflash::touch
