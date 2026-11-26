#include "preflash/platform.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string_view>
#include <vector>

#ifdef __APPLE__
#include <crt_externs.h>
#include <libproc.h>
#include <mach-o/dyld.h>
#else
#include <linux/close_range.h>
#include <sys/syscall.h>
extern char** environ;
#endif

namespace preflash::platform {

std::string without_deleted_mark(std::string path) {
  static constexpr std::string_view kDeleted = " (deleted)";
  if (path.ends_with(kDeleted)) path.resize(path.size() - kDeleted.size());
  return path;
}

std::string executable_path() {
  char path[PATH_MAX];
#ifdef __APPLE__
  uint32_t size = sizeof path;
  if (_NSGetExecutablePath(path, &size) != 0) return "";
  return path;
#else
  const ssize_t got = readlink("/proc/self/exe", path, sizeof path - 1);
  // A path that fills the buffer was cut short.
  if (got <= 0 || got == static_cast<ssize_t>(sizeof path - 1)) return "";
  path[got] = '\0';
  // Install renames the running binary aside before it stops the app, so the kernel marks
  // the link as deleted. A restart wants the new file that now sits at the original path.
  return without_deleted_mark(path);
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
    // A descriptor that's already closed has nothing to keep.
    if (keep[i] >= 0 && fcntl(keep[i], F_SETFD, 0) == -1 && errno != EBADF) return false;
  }
  return true;
#endif
}

std::string peer(int fd) {
#ifdef __APPLE__
  pid_t pid = 0;
  socklen_t size = sizeof pid;
  if (getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &pid, &size) != 0 || pid <= 0) return "";
  char name[2 * MAXCOMLEN + 1] = {};
  proc_name(pid, name, sizeof name);
  return "pid " + std::to_string(pid) + (name[0] ? std::string(", ") + name : "");
#else
  ucred credentials{};
  socklen_t size = sizeof credentials;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &size) != 0 || credentials.pid <= 0) {
    return "";
  }
  std::string name;
  std::ifstream in("/proc/" + std::to_string(credentials.pid) + "/comm");
  std::getline(in, name);
  return "pid " + std::to_string(credentials.pid) + (name.empty() ? "" : ", " + name);
#endif
}

void ignore_hang_ups(int fd) {
#ifdef SO_NOSIGPIPE
  const int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#else
  (void)fd;
#endif
}

ssize_t send_some(int fd, const void* data, size_t size) {
#ifdef __APPLE__
  return write(fd, data, size);
#else
  return send(fd, data, size, MSG_NOSIGNAL);
#endif
}

}  // namespace preflash::platform
