// Runs the serial pump in a process of its own, because it swaps fds 1 and 2. The
// argument picks how the process ends, each right after filling the pump's pipe, so that
// a path that doesn't drain it loses the last thing written.
#include <fcntl.h>
#include <signal.h>
#include <sys/resource.h>
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

// Leaves too few descriptors for the pump to save both stdout and stderr.
int run_out_of_fds() {
  rlimit limit;
  getrlimit(RLIMIT_NOFILE, &limit);
  limit.rlim_cur = 64;
  setrlimit(RLIMIT_NOFILE, &limit);
  int last = -1;
  for (int fd; (fd = open("/dev/null", O_RDONLY)) >= 0;) last = fd;
  // Room for the pipe, one spare while each end moves, and stdout's copy.
  for (int i = 0; i < 3; ++i) close(last - i);
  if (preflash::serial::start()) return 6;
  dprintf(STDOUT_FILENO, "start refused\n");
  if (fcntl(STDERR_FILENO, F_GETFD) != -1) dprintf(STDERR_FILENO, "stderr still open\n");
  return 0;
}

// A pipe nobody reads, as with a terminal stopped by ^S.
int flood_a_stuck_stdout() {
  int ends[2];
  if (pipe(ends) != 0) return 7;
  const int report = dup(STDOUT_FILENO);
  alarm(20);  // a pump that blocks would hold the flood here
  dup2(ends[1], STDOUT_FILENO);
  if (!preflash::serial::start()) return 8;
  flood();
  flood();
  flood();
  dprintf(report, "flood finished\n");
  // Reading lets the writer through, and it says what it dropped.
  std::string tail;
  bool noticed = false;
  char buffer[65536];
  fcntl(ends[0], F_SETFL, O_NONBLOCK);
  for (int i = 0; i < 2000 && !noticed; ++i) {
    const ssize_t got = read(ends[0], buffer, sizeof buffer);
    if (got > 0) {
      tail.append(buffer, static_cast<size_t>(got));
      noticed = tail.find("dropped") != std::string::npos;
      tail.erase(0, tail.size() > 16 ? tail.size() - 16 : 0);
    }
    usleep(1000);
  }
  if (noticed) dprintf(report, "notice seen\n");
  _exit(0);
}

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
  if (mode == "no_fds") return run_out_of_fds();
  if (mode == "stalled") return flood_a_stuck_stdout();
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
