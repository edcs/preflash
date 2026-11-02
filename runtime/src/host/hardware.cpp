#include "preflash/hardware.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include "preflash/bus.h"
#include "preflash/net.h"
#include "preflash/serial.h"
#include "preflash/socket.h"

namespace preflash::hardware {
namespace {

namespace fs = std::filesystem;
using nlohmann::json;

constexpr int kAddresses = 128;
constexpr const char* kFaultsFile = "faults.json";
constexpr const char* kUnplugged = "unplugged";
constexpr const char* kNoBattery = "none";
constexpr const char* kHealthy = "healthy";
constexpr const char* kAllChips = "all";

struct Option {
  std::string value;
  std::string label;
};

// The first option is how a healthy board comes.
struct Item {
  std::string name;
  std::string label;
  std::string group;
  bool live;
  std::vector<Option> options;
};

std::mutex g_mutex;
Board g_board;
std::vector<Item> g_items;
std::map<std::string, std::string> g_now;   // differs from the first option only
std::map<std::string, std::string> g_next;  // the same, for the next boot

std::atomic<int> g_rotation{0};
std::atomic<bool> g_usb_in{true};
std::atomic<int> g_battery{-1};
std::atomic<bool> g_rtc_flat{false};
std::array<std::array<std::atomic<bool>, kAddresses>, kI2cPorts> g_nacks{};

std::string capitals(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
  return text;
}

bool has_chip(const Board& board, const char* chip) {
  return std::any_of(board.i2c.begin(), board.i2c.end(),
                     [chip](const ChipEntry& entry) { return entry.chip == chip; });
}

std::vector<Item> items_for(const Board& board) {
  std::vector<Item> items;
  if (has_chip(board, "qmi8658")) {
    items.push_back({"rotation", "Rotation", "", true,
                     {{"0", "0°"}, {"90", "90°"}, {"180", "180°"}, {"270", "270°"}}});
  }
  if (has_chip(board, "axp2101")) {
    items.push_back({"usb", "USB power", "", true, {{"plugged", "plugged in"}, {kUnplugged, kUnplugged}}});
    items.push_back({"battery", "Battery", "", true,
                     {{kNoBattery, "not fitted"}, {"100", "100%"}, {"50", "50%"}, {"15", "15%"}, {"0", "0%"}}});
  }
  items.push_back({"wifi", "Wi-Fi", "Network", true, {{"mac", "Mac's connection"}}});
  items.push_back({"internet", "Internet", "Network", true, {{"real", "real"}, {"offline", "offline"}}});
  if (has_chip(board, "pcf85063")) {
    items.push_back({"rtc", "RTC", "Break something", false, {{kHealthy, kHealthy}, {"flat", "flat backup cell"}}});
  }
  if (!board.i2c.empty()) {
    Item i2c{"i2c", "I2C", "Break something", true, {{kHealthy, kHealthy}, {kAllChips, "NACK all"}}};
    std::set<std::string> seen;
    for (const ChipEntry& entry : board.i2c) {
      if (seen.insert(entry.chip).second) i2c.options.push_back({entry.chip, "NACK " + capitals(entry.chip)});
    }
    items.push_back(std::move(i2c));
  }
  if (has_chip(board, "qmi8658")) {
    items.push_back({"motion", "Motion sensor", "Break something", false, {{kHealthy, kHealthy}, {"gone", "gone"}}});
  }
  items.push_back({"wifi_join", "Wi-Fi join", "Break something", true, {{"succeeds", "succeeds"}, {"fails", "fails"}}});
  return items;
}

const Item* find_item(const std::string& name) {
  const auto it = std::find_if(g_items.begin(), g_items.end(),
                               [&name](const Item& item) { return item.name == name; });
  return it == g_items.end() ? nullptr : &*it;
}

bool takes(const Item& item, const std::string& value) {
  return std::any_of(item.options.begin(), item.options.end(),
                     [&value](const Option& option) { return option.value == value; });
}

std::string value_in(const std::map<std::string, std::string>& values, const Item& item) {
  const auto it = values.find(item.name);
  return it == values.end() ? item.options.front().value : it->second;
}

std::string now_value(const std::string& name) {
  const Item* item = find_item(name);
  return item ? value_in(g_now, *item) : "";
}

void store(std::map<std::string, std::string>* values, const Item& item, const std::string& value) {
  if (value == item.options.front().value) {
    values->erase(item.name);
  } else {
    (*values)[item.name] = value;
  }
}

std::optional<fs::path> faults_path() {
  const char* state = std::getenv("PREFLASH_STATE");
  if (!state || !*state) return std::nullopt;
  return fs::path(state) / kFaultsFile;
}

// Beside the target and renamed, so a crash never leaves half a file.
bool save_faults() {
  const auto path = faults_path();
  if (!path) return false;
  const fs::path temp = path->string() + ".tmp";
  std::error_code ec;
  fs::create_directories(path->parent_path(), ec);
  {
    std::ofstream out(temp, std::ios::trunc);
    out << json(g_next).dump() << '\n';
    out.close();
    if (!out) return false;
  }
  fs::rename(temp, *path, ec);
  return !ec;
}

std::map<std::string, std::string> saved_faults() {
  const auto path = faults_path();
  std::map<std::string, std::string> saved;
  if (!path) return saved;
  std::ifstream in(*path);
  if (!in) return saved;
  const json file = json::parse(in, nullptr, false);
  if (!file.is_object()) return saved;
  for (const auto& [name, value] : file.items()) {
    const Item* item = find_item(name);
    if (item && value.is_string() && takes(*item, value.get<std::string>())) {
      store(&saved, *item, value.get<std::string>());
    }
  }
  return saved;
}

// Everything chip models and the network read, from g_now.
void apply() {
  const auto number_or = [](const std::string& text, int otherwise) {
    return text.empty() || text == kNoBattery ? otherwise : std::atoi(text.c_str());
  };
  g_rotation = number_or(now_value("rotation"), 0);
  g_usb_in = now_value("usb") != kUnplugged;
  g_battery = number_or(now_value("battery"), -1);
  g_rtc_flat = now_value("rtc") == "flat";
  wifi_force_join_failure(now_value("wifi_join") == "fails");
  net_set_online(now_value("internet") != "offline");

  const std::string nacked = now_value("i2c");
  const bool motion_gone = now_value("motion") == "gone";
  for (auto& port : g_nacks) {
    for (auto& address : port) address = false;
  }
  for (const ChipEntry& entry : g_board.i2c) {
    const bool forced = nacked == kAllChips || nacked == entry.chip;
    if (forced || (motion_gone && entry.chip == "qmi8658")) g_nacks[entry.bus][entry.address] = true;
  }
}

std::string choices(const Item& item) {
  std::string out;
  for (size_t i = 0; i < item.options.size(); ++i) {
    if (i > 0) out += i + 1 == item.options.size() ? " or " : ", ";
    out += item.options[i].value;
  }
  return out;
}

}  // namespace

void load(const Board& board) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_board = board;
  g_items = items_for(board);
  g_next = saved_faults();
  g_now = g_next;
  apply();
}

std::string forced_summary() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::string out;
  for (const Item& item : g_items) {
    const std::string value = value_in(g_now, item);
    if (value == item.options.front().value) continue;
    const auto option = std::find_if(item.options.begin(), item.options.end(),
                                     [&value](const Option& o) { return o.value == value; });
    if (!out.empty()) out += ", ";
    out += item.label + " " + option->label;
  }
  return out;
}

json describe() {
  std::lock_guard<std::mutex> lock(g_mutex);
  json items = json::array();
  for (const Item& item : g_items) {
    json options = json::array();
    for (const Option& option : item.options) {
      options.push_back({{"value", option.value}, {"label", option.label}});
    }
    items.push_back({{"name", item.name},
                     {"label", item.label},
                     {"group", item.group},
                     {"applies", item.live ? "now" : "boot"},
                     {"value", value_in(g_now, item)},
                     {"next", value_in(g_next, item)},
                     {"options", options}});
  }
  return items;
}

std::string set(const std::string& name, const std::string& value) {
  std::lock_guard<std::mutex> lock(g_mutex);
  const Item* item = find_item(name);
  if (!item) return "no hardware called " + name;
  if (!takes(*item, value)) return name + " takes " + choices(*item);
  const bool no_usb = name == "usb" ? value == kUnplugged : now_value("usb") == kUnplugged;
  const bool no_battery = name == "battery" ? value == kNoBattery : now_value("battery") == kNoBattery;
  if (no_usb && no_battery) return "with no USB and no battery, the board has no power";
  if (!item->live && !faults_path()) return "this app has no state folder to keep that in";

  store(&g_next, *item, value);
  if (item->live) {
    store(&g_now, *item, value);
    apply();
  }
  if (faults_path() && !save_faults()) return "couldn't save " + std::string(kFaultsFile);
  return "";
}

int rotation() { return g_rotation; }

bool usb_in() { return g_usb_in; }

int battery_percent() { return g_battery; }

bool rtc_flat() { return g_rtc_flat; }

bool i2c_nacks(int port, uint8_t address) {
  return g_nacks[port][address & (kAddresses - 1)].load(std::memory_order_relaxed);
}

void switch_off() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (const Item* usb = find_item("usb")) {
      store(&g_next, *usb, usb->options.front().value);
      if (faults_path()) save_faults();
    }
  }
  serial::notice("preflash: the power chip switched the board off\n");
  socket::quit();
  std::_Exit(0);
}

}  // namespace preflash::hardware
