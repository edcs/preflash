#include "pad.h"

#include <algorithm>
#include <utility>

namespace preflash::window {

void Pad::press(const std::string& name) {
  if (std::find(held_.begin(), held_.end(), name) == held_.end()) held_.push_back(name);
}

void Pad::release(const std::string& name) {
  held_.erase(std::remove(held_.begin(), held_.end(), name), held_.end());
}

std::vector<std::string> Pad::release_all() { return std::exchange(held_, {}); }

void Pad::keep_only(const std::vector<std::string>& names) {
  held_.erase(std::remove_if(held_.begin(), held_.end(),
                             [&names](const std::string& name) {
                               return std::find(names.begin(), names.end(), name) == names.end();
                             }),
              held_.end());
}

}  // namespace preflash::window
