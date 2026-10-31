#include "preflash/board.h"

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace preflash {
namespace {

using nlohmann::json;

[[noreturn]] void bad(const std::string& field, const char* what) {
  throw std::runtime_error("board: " + field + " " + what);
}

const json& member(const json& obj, const std::string& parent, const char* key) {
  std::string field = parent.empty() ? key : parent + "." + key;
  if (!obj.is_object()) bad(parent, "must be an object");
  auto it = obj.find(key);
  if (it == obj.end()) bad(field, "is missing");
  return *it;
}

int integer(const json& obj, const std::string& parent, const char* key) {
  const json& v = member(obj, parent, key);
  if (!v.is_number_integer()) bad(parent.empty() ? key : parent + "." + key, "must be an integer");
  return v.get<int>();
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

  const json& chips = member(root, "", "i2c");
  if (!chips.is_array()) bad("i2c", "must be a list");
  for (size_t i = 0; i < chips.size(); i++) {
    std::string where = "i2c[" + std::to_string(i) + "]";
    int address = integer(chips[i], where, "address");
    if (address < 0 || address > 0x7f) bad(where + ".address", "must be a 7-bit address");
    b.i2c.push_back({text(chips[i], where, "chip"), static_cast<uint8_t>(address)});
  }

  const json& bl = member(root, "", "backlight");
  b.backlight.pin = integer(bl, "backlight", "pin");
  const json& low = member(bl, "backlight", "active_low");
  if (!low.is_boolean()) bad("backlight.active_low", "must be true or false");
  b.backlight.active_low = low.get<bool>();
  if (bl.contains("enable_pin")) b.backlight.enable_pin = integer(bl, "backlight", "enable_pin");

  b.audio_codec = text(root, "", "audio_codec");
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
