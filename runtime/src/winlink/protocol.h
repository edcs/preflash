#pragma once

#include <optional>
#include <string>

#include <nlohmann/json.hpp>

// Newline-delimited JSON between the app and its window process. Every message has a string
// `op`.
//
// App to window:
//   board {project, board, width, height, buttons:[{name, wiring}], frame_path}
//   state {power:"on"|"off"|"download", backlight:0-100, rotation:0|90|180|270, held:[names]}
//                           rotation is the board's, clockwise, which the window turns by
//   close {}
// Window to app:
//   touch {x, y, down}      panel coordinates
//   button {name, down}
//   rotate {by: 90|-90}
//   quit {}
//   hello {held:[names]}    sent on each connect
namespace preflash::winlink {

// One line, ending in a single "\n".
std::string encode(const nlohmann::json& message);
// Nullopt for bad JSON, or when `op` is missing or isn't a string. Takes a line with or
// without its trailing newline.
std::optional<nlohmann::json> decode_line(const std::string& line);

}  // namespace preflash::winlink
