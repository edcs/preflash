#include "preflash/replay.h"

#include <cstdint>
#include <cstdio>
#include <istream>
#include <nlohmann/json.hpp>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "preflash/bus.h"
#include "preflash/fail.h"

namespace preflash {
namespace {

using nlohmann::json;

struct Refused : std::runtime_error {
  using std::runtime_error::runtime_error;
};

[[noreturn]] void refuse(const char* call, const char* why) {
  throw Refused(std::string(call) + ": " + why);
}

std::vector<uint8_t> from_hex(const std::string& text) {
  if (text.size() % 2 != 0) throw std::runtime_error("odd hex: " + text);
  std::vector<uint8_t> bytes;
  for (size_t i = 0; i < text.size(); i += 2) {
    bytes.push_back(static_cast<uint8_t>(std::stoul(text.substr(i, 2), nullptr, 16)));
  }
  return bytes;
}

std::string to_hex(std::span<const uint8_t> bytes) {
  std::string text;
  char byte[3];
  for (const uint8_t value : bytes) {
    std::snprintf(byte, sizeof byte, "%02x", value);
    text += byte;
  }
  return text;
}

json answer(const json& transaction) {
  const std::vector<uint8_t> write = from_hex(transaction.at("write").get<std::string>());
  std::vector<uint8_t> read(transaction.at("read_len").get<size_t>());
  I2cBus& bus = i2c_bus(transaction.at("bus").get<int>());
  const auto address = transaction.at("addr").get<uint8_t>();
  try {
    const bool acked = bus.transfer(address, write, read);
    return {{"acked", acked}, {"read", acked ? to_hex(read) : ""}};
  } catch (const Refused& refused) {
    return {{"acked", false}, {"read", ""}, {"refused", refused.what()}};
  }
}

// Restores the usual handler however a replay ends.
struct RefusalsThrow {
  RefusalsThrow() { set_unsupported_handler(refuse); }
  ~RefusalsThrow() { set_unsupported_handler(nullptr); }
};

}  // namespace

void replay(std::istream& in, std::ostream& out) {
  RefusalsThrow refusals_throw;
  for (std::string line; std::getline(in, line);) {
    out << answer(json::parse(line)).dump() << std::endl;
  }
}

}  // namespace preflash
