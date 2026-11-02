// Runs the serial pump in a process of its own, because it swaps fds 1 and 2. The
// argument picks how the process ends, each right after filling the pump's pipe, so that
// a path that doesn't drain it loses the last thing written.
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "esp_system.h"
#include "preflash/fail.h"
#include "preflash/serial.h"
#include "preflash/socket.h"

namespace {

void flood() {
  const std::string line(99, 'x');
  for (int i = 0; i < 20000; ++i) std::printf("%s\n", line.c_str());
}

ino_t inode(int fd) {
  struct stat info {};
  fstat(fd, &info);
  return info.st_ino;
}

void on_interrupt(int) { preflash::socket::leave(0); }

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  setvbuf(stdout, nullptr, _IOLBF, 0);
  const std::string mode = argv[1];
  const std::string marker = std::string(std::getenv("PREFLASH_STATE")) + "/restarted";

  if (mode == "restart" && access(marker.c_str(), F_OK) == 0) {
    const bool out = std::to_string(inode(1)) == std::getenv("HELPER_OUT");
    const bool err = std::to_string(inode(2)) == std::getenv("HELPER_ERR");
    std::printf("second run: stdout %s, stderr %s\n", out ? "same" : "different",
                err ? "same" : "different");
    return 0;
  }

  if (mode == "restart") {
    setenv("HELPER_OUT", std::to_string(inode(1)).c_str(), 1);
    setenv("HELPER_ERR", std::to_string(inode(2)).c_str(), 1);
  }
  if (mode == "interrupt") signal(SIGINT, on_interrupt);
  if (!preflash::serial::start()) return 3;

  if (mode == "subscribe") {
    static std::mutex mutex;
    static std::string seen;
    std::printf("before\n");
    // The pump gets to it in its own time, so this waits for the ring to hold it.
    for (int i = 0; i < 1000; ++i) {
      if (!preflash::serial::ring_contents().empty()) break;
      usleep(1000);
    }
    preflash::serial::subscribe([](const char* data, size_t size) {
      std::lock_guard<std::mutex> lock(mutex);
      seen.append(data, size);
    });
    std::printf("after out\n");
    std::fprintf(stderr, "after err\n");
    preflash::serial::hand_back();
    std::lock_guard<std::mutex> lock(mutex);
    std::printf("subscriber saw: %s;\n", seen.c_str());
    return 0;
  }

  if (mode == "binary") {
    const char bytes[] = {'\xff', '\0', '\xfe', '\n'};
    write(STDOUT_FILENO, bytes, sizeof bytes);
    preflash::socket::quit();
  }

  flood();
  if (mode == "unsupported") {
    std::printf("end of flood\n");
    preflash_unsupported("flash_op", "because");
  }
  if (mode == "quit") {
    std::printf("end of flood, no newline");
    preflash::socket::quit();
  }
  std::printf("end of flood\n");
  if (mode == "abort") std::abort();
  if (mode == "crash") raise(SIGSEGV);
  if (mode == "interrupt") {
    raise(SIGINT);
    return 4;
  }
  if (mode == "restart") {
    FILE* made = std::fopen(marker.c_str(), "w");
    if (!made) return 5;
    std::fclose(made);
    esp_restart();
  }
  return 6;
}
