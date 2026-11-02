#include "preflash/board.h"

#include <climits>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "preflash/bus.h"

namespace preflash {
namespace {

using nlohmann::json;

[[noreturn]] void bad(const std::string& field, const char* what) {
  throw std::runtime_error("board: " + field + " " + what);
}

const json& member(const json& obj, const std::string& parent, const char* key) {
  std::string field = parent.empty() ? key : parent + "." + key;
  if (!obj.is_object()) bad(parent.empty() ? "root" : parent, "must be an object");
  auto it = obj.find(key);
  if (it == obj.end()) bad(field, "is missing");
  return *it;
}

int integer(const json& obj, const std::string& parent, const char* key) {
  const json& v = member(obj, parent, key);
  if (!v.is_number_integer()) bad(parent.empty() ? key : parent + "." + key, "must be an integer");
  // The parser gives non-negative numbers as unsigned and negative ones as signed;
  // each is checked on 64 bits so that none can wrap into range.
  bool fits = v.is_number_unsigned() ? v.get<std::uint64_t>() <= static_cast<std::uint64_t>(INT_MAX)
                                     : v.get<std::int64_t>() >= INT_MIN && v.get<std::int64_t>() <= INT_MAX;
  if (!fits) bad(parent.empty() ? key : parent + "." + key, "is out of range for an integer");
  return v.get<int>();
}

// The bit of the 8-pin expander, if the object names one.
std::optional<int> expander_bit(const json& obj, const std::string& parent, const char* key) {
  if (!obj.contains(key)) return std::nullopt;
  const int bit = integer(obj, parent, key);
  if (bit < 0 || bit > 7) bad(parent.empty() ? key : parent + "." + key, "must be a bit from 0 to 7");
  return bit;
}

std::string text(const json& obj, const std::string& parent, const char* key) {
  const json& v = member(obj, parent, key);
  if (!v.is_string()) bad(parent.empty() ? key : parent + "." + key, "must be a string");
  return v.get<std::string>();
}

}  // namespace

Board parse_board(const std::string& source) {
  json root = json::parse(source, nullptr, false);
  if (root.is_discarded()) throw std::runtime_error("board: the JSON does not parse");

  Board b;
  b.name = text(root, "", "name");

  const json& display = member(root, "", "display");
  b.width = integer(display, "display", "width");
  b.height = integer(display, "display", "height");
  if (b.width <= 0) bad("display.width", "must be positive");
  if (b.height <= 0) bad("display.height", "must be positive");
  if (display.contains("controller")) b.controller = text(display, "display", "controller");
  b.big_endian = false;
  if (display.contains("byte_order")) {
    const std::string order = text(display, "display", "byte_order");
    if (order != "le" && order != "be") bad("display.byte_order", "must be le or be");
    b.big_endian = order == "be";
  }

  const json& chips = member(root, "", "i2c");
  if (!chips.is_array()) bad("i2c", "must be a list");
  for (size_t i = 0; i < chips.size(); i++) {
    std::string where = "i2c[" + std::to_string(i) + "]";
    int address = integer(chips[i], where, "address");
    if (address < 0 || address > 0x7f) bad(where + ".address", "must be a 7-bit address");
    int bus = 0;
    if (chips[i].contains("bus")) bus = integer(chips[i], where, "bus");
    if (bus < 0 || bus >= kI2cPorts) bad(where + ".bus", "must be 0 or 1");
    b.i2c.push_back({text(chips[i], where, "chip"), static_cast<uint8_t>(address), bus});
  }

  const json& bl = member(root, "", "backlight");
  b.backlight.pin = integer(bl, "backlight", "pin");
  const json& low = member(bl, "backlight", "active_low");
  if (!low.is_boolean()) bad("backlight.active_low", "must be true or false");
  b.backlight.active_low = low.get<bool>();
  if (bl.contains("enable_pin")) b.backlight.enable_pin = integer(bl, "backlight", "enable_pin");
  b.backlight.enable_expander_bit = expander_bit(bl, "backlight", "enable_expander_bit");

  b.audio_codec = text(root, "", "audio_codec");
  b.power_amp_bit = expander_bit(root, "", "power_amp_bit");
  return b;
}

const Board& board() {
  static const Board loaded = [] {
    const char* path = std::getenv("PREFLASH_BOARD");
    if (!path || !*path) throw std::runtime_error("board: PREFLASH_BOARD is not set");
    std::ifstream in(path);
    if (!in) throw std::runtime_error(std::string("board: cannot read ") + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return parse_board(ss.str());
  }();
  return loaded;
}

}  // namespace preflash
