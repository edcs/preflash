#include "pad.h"

#include <algorithm>

namespace preflash::window {
namespace {

bool contains(const std::vector<std::string>& names, const std::string& name) {
  return std::find(names.begin(), names.end(), name) != names.end();
}

void erase(std::vector<std::string>* names, const std::string& name) {
  names->erase(std::remove(names->begin(), names->end(), name), names->end());
}

}  // namespace

void Pad::press(const std::string& name) {
  if (!contains(held_, name)) held_.push_back(name);
}

void Pad::latch(const std::string& name) {
  press(name);
  if (!contains(latched_, name)) latched_.push_back(name);
}

void Pad::release(const std::string& name) {
  erase(&held_, name);
  erase(&latched_, name);
}

bool Pad::latched(const std::string& name) const { return contains(latched_, name); }

std::vector<std::string> Pad::release_all() {
  std::vector<std::string> let_go;
  for (const std::string& name : held_) {
    if (!latched(name)) let_go.push_back(name);
  }
  held_.erase(std::remove_if(held_.begin(), held_.end(),
                             [this](const std::string& name) { return !latched(name); }),
              held_.end());
  return let_go;
}

void Pad::keep_only(const std::vector<std::string>& names) {
  const auto lacks = [&names](const std::string& name) { return !contains(names, name); };
  held_.erase(std::remove_if(held_.begin(), held_.end(), lacks), held_.end());
  latched_.erase(std::remove_if(latched_.begin(), latched_.end(), lacks), latched_.end());
}

}  // namespace preflash::window
