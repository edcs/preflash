#include "winlink/protocol.h"

namespace preflash::winlink {

std::string encode(const nlohmann::json& message) {
  // Replace mode keeps invalid UTF-8 in a name from throwing out of the send path.
  return message.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

std::optional<nlohmann::json> decode_line(const std::string& line) {
  auto message = nlohmann::json::parse(line, nullptr, false);
  if (message.is_discarded() || !message.is_object()) return std::nullopt;
  auto op = message.find("op");
  if (op == message.end() || !op->is_string()) return std::nullopt;
  return message;
}

}  // namespace preflash::winlink
