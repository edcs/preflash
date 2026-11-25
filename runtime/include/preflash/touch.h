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

// The panel's pointer, in the panel's own coordinates: a press, a move while pressed, or a
// release. A point off the panel lands on its nearest edge. Nothing happens once firmware has
// crashed, since nothing reads touches then. Returns at once. The window and the socket both
// come through here, and nothing else queues a pointer's touches.
void pointer(int x, int y, bool down);

}  // namespace preflash::touch
