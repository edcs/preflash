#pragma once

#include <string>
#include <vector>

namespace preflash::window {

// The board buttons the window holds down, in the order they went down. The window tells the
// app of each change itself; this only keeps count. A latched button stays down without the
// mouse, until it's let go of by name.
class Pad {
 public:
  void press(const std::string& name);
  // Holds a button down until release, through focus loss and the pointer leaving.
  void latch(const std::string& name);
  void release(const std::string& name);
  bool latched(const std::string& name) const;
  // Lets go of everything the mouse holds, as when the window loses the mouse, and says what
  // it let go. Latched buttons stay down.
  std::vector<std::string> release_all();
  std::vector<std::string> held() const { return held_; }
  // Forgets buttons a new board doesn't have.
  void keep_only(const std::vector<std::string>& names);

 private:
  std::vector<std::string> held_;
  std::vector<std::string> latched_;
};

}  // namespace preflash::window
