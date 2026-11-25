#include "preflash/platform.h"

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <vector>

#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#else
#include <linux/close_range.h>
#include <sys/syscall.h>
extern char** environ;
#endif

namespace preflash::platform {

std::string executable_path() {
  char path[PATH_MAX];
#ifdef __APPLE__
  uint32_t size = sizeof path;
  if (_NSGetExecutablePath(path, &size) != 0) return "";
  return path;
#else
  const ssize_t got = readlink("/proc/self/exe", path, sizeof path - 1);
  if (got <= 0) return "";
  path[got] = '\0';
  return path;
#endif
}

char** arguments() {
#ifdef __APPLE__
  return *_NSGetArgv();
#else
  // Read once and kept: an exec needs them, and the process ends there.
  static std::vector<std::string> words;
  static std::vector<char*> pointers;
  if (pointers.empty()) {
    std::ifstream in("/proc/self/cmdline", std::ios::binary);
    std::string word;
    while (std::getline(in, word, '\0')) words.push_back(word);
    for (std::string& w : words) pointers.push_back(w.data());
    pointers.push_back(nullptr);
  }
  return pointers.data();
#endif
}

char** environment() {
#ifdef __APPLE__
  return *_NSGetEnviron();
#else
  return environ;
#endif
}

bool close_on_exec_by_listing() {
  DIR* dir = opendir("/proc/self/fd");
  if (!dir) return false;
  const int own = dirfd(dir);
  bool marked = true;
  while (const dirent* entry = readdir(dir)) {
    char* end = nullptr;
    const long fd = std::strtol(entry->d_name, &end, 10);
    // "." and "..", the standard three, and the listing's own descriptor, which closedir
    // closes anyway.
    if (end == entry->d_name || *end != '\0' || fd < 3 || fd == own) continue;
    const int flags = fcntl(static_cast<int>(fd), F_GETFD);
    if (flags == -1 || fcntl(static_cast<int>(fd), F_SETFD, flags | FD_CLOEXEC) == -1) {
      marked = false;
    }
  }
  closedir(dir);
  return marked;
}

bool close_on_exec_except(const int* keep, int count) {
#ifdef __APPLE__
  (void)keep;
  (void)count;
  return true;
#else
  // CLOSE_RANGE_CLOEXEC needs Linux 5.11, and some container seccomp profiles refuse
  // close_range outright, so a failure falls back to marking each open descriptor by hand.
  if (syscall(SYS_close_range, 3u, ~0u, CLOSE_RANGE_CLOEXEC) != 0 && !close_on_exec_by_listing()) {
    return false;
  }
  for (int i = 0; i < count; ++i) {
    if (keep[i] >= 0 && fcntl(keep[i], F_SETFD, 0) == -1) return false;
  }
  return true;
#endif
}

}  // namespace preflash::platform
