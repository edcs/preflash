#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace preflash {

struct TaskInfo {
  std::string name;
  const char* state;  // "running" or "blocked"
  uint32_t stack;     // what firmware declared. A host thread's use isn't measured
};

// The tasks xTaskCreate started that are still alive.
std::vector<TaskInfo> task_list();

// Marks the calling task as blocked for as long as this lives.
class Blocked {
 public:
  Blocked();
  ~Blocked();
  Blocked(const Blocked&) = delete;
  Blocked& operator=(const Blocked&) = delete;
};

}  // namespace preflash
