#include <cstdio>
#include <utility>

#include "preflash/chips.h"
#include "preflash/fail.h"

namespace preflash {

RegisterChip::RegisterChip(std::string name) : name_(std::move(name)) {}

bool RegisterChip::write(std::span<const uint8_t> data) {
  if (data.empty()) return true;
  address_ = data[0];
  for (const uint8_t byte : data.subspan(1)) {
    store(address_, byte);
    address_ = next(address_);
  }
  return true;
}

bool RegisterChip::read(std::span<uint8_t> out) {
  for (uint8_t& byte : out) {
    byte = load(address_);
    address_ = next(address_);
  }
  return true;
}

void RegisterChip::refuse(uint8_t reg, const char* why) const {
  char number[8];
  std::snprintf(number, sizeof number, "0x%02x", reg);
  const std::string call = name_ + " register " + number;
  preflash_unsupported(call.c_str(), why);
}

}  // namespace preflash
