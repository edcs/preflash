#include "preflash/buttons.h"

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <sstream>

#include "preflash/bus.h"
#include "preflash/trace.h"

namespace preflash::buttons {
namespace {

std::mutex g_mutex;
std::vector<Button> g_buttons;
std::vector<std::string> g_held;
std::vector<std::function<void(const std::string&, bool)>> g_listeners;

bool is_held(const std::string& name) {
  return std::find(g_held.begin(), g_held.end(), name) != g_held.end();
}

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
  const char* env = std::getenv("PREFLASH_HELD");
  std::stringstream names(env ? env : "");
  for (std::string name; std::getline(names, name, ',');) {
    const bool known = std::any_of(g_buttons.begin(), g_buttons.end(),
                                   [&name](const Button& b) { return b.name == name; });
    if (known && !is_held(name)) g_held.push_back(name);
  }
  for (const Button& button : g_buttons) drive(button, is_held(button.name));
}

std::string set(const std::string& name, bool down) {
  std::vector<std::function<void(const std::string&, bool)>> listeners;
  std::string wiring_text;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto button = std::find_if(g_buttons.begin(), g_buttons.end(),
                                     [&name](const Button& b) { return b.name == name; });
    if (button == g_buttons.end()) return no_such_button(name);
    if (is_held(name) == down) return "";
    if (down) {
      g_held.push_back(name);
    } else {
      g_held.erase(std::remove(g_held.begin(), g_held.end(), name), g_held.end());
    }
    drive(*button, down);
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
  for (const std::string& name : g_held) text += (text.empty() ? "" : ",") + name;
  return text;
}

}  // namespace preflash::buttons
