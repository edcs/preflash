#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <utility>

#include "preflash/chips.h"

namespace preflash {
namespace {

ChipFactory plain_chip(const char* name) {
  return [name] { return std::make_unique<RegisterChip>(name); };
}

// Listed here rather than registered from each chip's own file, because the
// linker drops objects from a static library when nothing refers to them.
std::map<std::string, ChipFactory>& factories() {
  static std::map<std::string, ChipFactory> known = {
      {"pcf85063", [] { return std::make_unique<Pcf85063>(); }},
      {"qmi8658", [] { return std::make_unique<Qmi8658>(); }},
      {"axp2101", [] { return std::make_unique<Axp2101>(); }},
      // These only need to answer. The audio chips are modelled at the API
      // level, and touches come from the mouse rather than the GT911.
      {"tca9554", plain_chip("tca9554")},
      {"es8311", plain_chip("es8311")},
      {"es7210", plain_chip("es7210")},
      {"gt911", plain_chip("gt911")},
  };
  return known;
}

}  // namespace

void register_chip(const std::string& name, ChipFactory factory) {
  factories()[name] = std::move(factory);
}

void attach_chips(const Board& board) {
  std::set<uint8_t> taken;
  for (const ChipEntry& entry : board.i2c) {
    const auto factory = factories().find(entry.chip);
    if (factory == factories().end())
      throw std::runtime_error("board: no model for chip " + entry.chip);
    if (!taken.insert(entry.address).second)
      throw std::runtime_error("board: chip " + entry.chip + " shares an address with another");
    i2c_bus().add(entry.address, factory->second());
  }
}

}  // namespace preflash
