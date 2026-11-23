#include "preflash/buttons.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <utility>

#include "preflash/bus.h"
#include "preflash/trace.h"

namespace preflash::buttons {
namespace {

struct Hold {
  std::string name;
  Owner owner;
  bool operator==(const Hold& other) const { return name == other.name && owner == other.owner; }
  // When a tap lets go, in CLOCK_REALTIME ms, so that a run that cuts it off can tell the next.
  std::optional<int64_t> release_at = std::nullopt;
};

std::mutex g_mutex;
std::vector<Button> g_buttons;
std::vector<std::string> g_held;  // down for anyone, in the order they went down
std::vector<Hold> g_holds;        // each owner's holds, in the order they were made
std::vector<std::function<void(const std::string&, bool)>> g_listeners;
// Counts loads, so that a release a load scheduled doesn't outlive the next load.
uint64_t g_loads = 0;
// Set by cut_taps, as the run ends, and cleared by load. Taps wait on it, under g_mutex.
bool g_taps_cut = false;
std::condition_variable g_taps_wake;

int64_t realtime_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

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
  g_taps_cut = false;
  const uint64_t this_load = ++g_loads;
  std::vector<Hold> timed;
  const char* env = std::getenv("PREFLASH_HELD");
  std::stringstream entries(env ? env : "");
  for (std::string entry; std::getline(entries, entry, ',');) {
    std::optional<int64_t> release_at;
    if (const size_t at = entry.rfind('@'); at != std::string::npos) {
      release_at = std::strtoll(entry.c_str() + at + 1, nullptr, 10);
      entry.resize(at);
    }
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
    g_holds.push_back({name, owner, release_at});
    if (release_at) timed.push_back(g_holds.back());
    if (!is_held(name)) g_held.push_back(name);
  }
  for (const Button& button : g_buttons) drive(button, is_held(button.name));
  // A tap the last run's exec cut off lets go when it would have, or now if that's past. On
  // threads of their own, as set calls listeners, and this holds the lock.
  for (const Hold& hold : timed) {
    try {
      std::thread([hold, this_load] {
        const int64_t wait = *hold.release_at - realtime_ms();
        if (wait > 0) std::this_thread::sleep_for(std::chrono::milliseconds(wait));
        {
          std::lock_guard<std::mutex> lock(g_mutex);
          const auto it = std::find(g_holds.begin(), g_holds.end(), hold);
          // Another load since, or the same owner let go and pressed again: not this tap.
          if (g_loads != this_load || it == g_holds.end() || it->release_at != hold.release_at) return;
        }
        set(hold.name, false, hold.owner);
      }).detach();
    } catch (const std::system_error&) {
      // No thread to wait on: let go at once, which beats never.
      g_holds.erase(std::remove(g_holds.begin(), g_holds.end(), hold), g_holds.end());
      if (std::none_of(g_holds.begin(), g_holds.end(),
                       [&hold](const Hold& h) { return h.name == hold.name; })) {
        g_held.erase(std::remove(g_held.begin(), g_held.end(), hold.name), g_held.end());
        for (const Button& button : g_buttons) {
          if (button.name == hold.name) drive(button, false);
        }
      }
    }
  }
}

namespace {

// *moved, if given, says whether the owner's hold changed: false when it already held the
// button down, or had already let go.
std::string change(const std::string& name, bool down, Owner owner,
                   std::optional<int64_t> release_at, bool* moved = nullptr) {
  if (moved) *moved = false;
  std::vector<std::function<void(const std::string&, bool)>> listeners;
  std::string wiring_text;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    const auto button = std::find_if(g_buttons.begin(), g_buttons.end(),
                                     [&name](const Button& b) { return b.name == name; });
    if (button == g_buttons.end()) return no_such_button(name);
    if (holds(name, owner) == down) return "";
    if (moved) *moved = true;
    if (down) {
      g_holds.push_back({name, owner, release_at});
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

}  // namespace

std::string set(const std::string& name, bool down, Owner owner) {
  return change(name, down, owner, std::nullopt);
}

std::string press_until(const std::string& name, Owner owner, int64_t release_at) {
  return change(name, true, owner, release_at);
}

std::string tap(const std::string& name, int ms, Owner owner) {
  // The deadline goes in with the press, before any listener can start an exec.
  bool pressed = false;
  if (std::string error = change(name, true, owner, realtime_ms() + ms, &pressed);
      !error.empty()) {
    return error;
  }
  // The owner already held it down, by a press or a tap under way: that hold isn't this tap's
  // to let go of.
  if (!pressed) return "";
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    // Cut off: the next run lets go at the deadline held_env() gave it, and this one answers.
    if (g_taps_wake.wait_for(lock, std::chrono::milliseconds(ms), [] { return g_taps_cut; })) {
      return "";
    }
  }
  return set(name, false, owner);
}

void cut_taps() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_taps_cut = true;
  }
  g_taps_wake.notify_all();
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
    if (hold.release_at) text += "@" + std::to_string(*hold.release_at);
  }
  return text;
}

}  // namespace preflash::buttons
