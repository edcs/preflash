#include "preflash/buttons.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <utility>

#include "preflash/bus.h"
#include "preflash/trace.h"

namespace preflash::buttons {
namespace {

struct Hold {
  std::string name;
  Owner owner;
  bool operator==(const Hold&) const = default;
};

std::mutex g_mutex;
std::vector<Button> g_buttons;
std::vector<std::string> g_held;  // down for anyone, in the order they went down
std::vector<Hold> g_holds;        // each owner's holds, in the order they were made
std::vector<std::function<void(const std::string&, bool)>> g_listeners;

bool is_held(const std::string& name) {
  return std::find(g_held.begin(), g_held.end(), name) != g_held.end();
}

bool holds(const std::string& name, Owner owner) {
  return std::find(g_holds.begin(), g_holds.end(), Hold{name, owner}) != g_holds.end();
}

const char* owner_name(Owner owner) { return owner == Owner::Window ? "window" : "script"; }

// Moves what the real button moves. A PWR on the AXP2101 and RST move nothing here: those
// who act on them hear of the change.
void drive(const Button& button, bool down) {
  const bool level = button.active_low ? !down : down;
  switch (button.kind) {
    case Button::Kind::Gpio:
    case Button::Kind::Power:
      gpio_write(button.pin, level);
      break;
    case Button::Kind::Expander:
      expander_drive(button.pin, level);
      break;
    case Button::Kind::Axp2101:
    case Button::Kind::Reset:
      break;
  }
}

std::string no_such_button(const std::string& name) {
  std::string names;
  for (const Button& button : g_buttons) names += (names.empty() ? "" : ", ") + button.name;
  return "no button named " + name + ": the board has " + (names.empty() ? "none" : names);
}

}  // namespace

void load(const Board& board) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_buttons = board.buttons;
  g_held.clear();
  g_holds.clear();
  const char* env = std::getenv("PREFLASH_HELD");
  std::stringstream entries(env ? env : "");
  for (std::string entry; std::getline(entries, entry, ',');) {
    const size_t colon = entry.rfind(':');
    const std::string name = entry.substr(0, colon);
    const Owner owner =
        colon != std::string::npos && entry.substr(colon + 1) == "window" ? Owner::Window
                                                                         : Owner::Script;
    // A reset button's hold never outlasts the run: the reset under way took the press.
    const bool known = std::any_of(g_buttons.begin(), g_buttons.end(), [&name](const Button& b) {
      return b.name == name && b.kind != Button::Kind::Reset;
    });
    if (!known || holds(name, owner)) continue;
    g_holds.push_back({name, owner});
    if (!is_held(name)) g_held.push_back(name);
  }
  for (const Button& button : g_buttons) drive(button, is_held(button.name));
}

std::string set(const std::string& name, bool down, Owner owner) {
  std::vector<std::function<void(const std::string&, bool)>> listeners;
  std::string wiring_text;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto button = std::find_if(g_buttons.begin(), g_buttons.end(),
                                     [&name](const Button& b) { return b.name == name; });
    if (button == g_buttons.end()) return no_such_button(name);
    if (holds(name, owner) == down) return "";
    if (down) {
      g_holds.push_back({name, owner});
    } else {
      g_holds.erase(std::remove(g_holds.begin(), g_holds.end(), Hold{name, owner}),
                    g_holds.end());
    }
    const bool now = std::any_of(g_holds.begin(), g_holds.end(),
                                 [&name](const Hold& h) { return h.name == name; });
    // Another owner still holds it, or already did: the button itself hasn't moved.
    if (now == is_held(name)) return "";
    if (now) {
      g_held.push_back(name);
    } else {
      g_held.erase(std::remove(g_held.begin(), g_held.end(), name), g_held.end());
    }
    drive(*button, now);
    listeners = g_listeners;
    wiring_text = wiring(*button);
  }
  if (trace::on()) {
    trace::note(trace::Stream::Button, name.c_str(), down ? "press" : "release", wiring_text);
  }
  // Outside the lock, so that a listener can ask what's held, or press something itself.
  for (const auto& listener : listeners) listener(name, down);
  return "";
}

bool down(const std::string& name) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return is_held(name);
}

std::vector<std::string> held() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_held;
}

std::vector<std::string> held_by(Owner owner) {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<std::string> names;
  for (const Hold& hold : g_holds) {
    if (hold.owner == owner) names.push_back(hold.name);
  }
  return names;
}

nlohmann::json describe() {
  std::lock_guard<std::mutex> lock(g_mutex);
  nlohmann::json list = nlohmann::json::array();
  for (const Button& button : g_buttons) {
    list.push_back(
        {{"name", button.name}, {"wiring", wiring(button)}, {"down", is_held(button.name)}});
  }
  return list;
}

void on_change(std::function<void(const std::string&, bool)> listener) {
  if (!listener) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  g_listeners.push_back(std::move(listener));
}

std::string held_env() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string text;
  for (const Hold& hold : g_holds) {
    text += (text.empty() ? "" : ",") + hold.name + ":" + owner_name(hold.owner);
  }
  return text;
}

}  // namespace preflash::buttons
