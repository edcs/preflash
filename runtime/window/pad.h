#pragma once

#include <string>
#include <vector>

namespace preflash::window {

// The board buttons the window holds down, in the order they went down. The window tells the
// app of each change itself; this only keeps count.
class Pad {
 public:
  void press(const std::string& name);
  void release(const std::string& name);
  // Lets go of everything, as when the window loses the mouse, and says what it let go.
  std::vector<std::string> release_all();
  std::vector<std::string> held() const { return held_; }
  // Forgets buttons a new board doesn't have.
  void keep_only(const std::vector<std::string>& names);

 private:
  std::vector<std::string> held_;
};

}  // namespace preflash::window
