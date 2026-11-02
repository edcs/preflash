#pragma once

#include <optional>

// Touches in the order they happened, so that a press and release that both
// arrive between two reads still reach firmware as two events.
namespace preflash::touch {

constexpr int kDefaultPressMs = 80;

struct Event {
  int x, y;
  bool down;
};

void push(Event e);
std::optional<Event> pop();

// Presses the panel and holds for ms before letting go, in the panel's own coordinates.
// Blocks the caller for that long, and any other press behind it, so that gestures stay
// whole. False, doing nothing, if the point is off the panel or ms is negative.
bool press(int x, int y, int ms);

}  // namespace preflash::touch
