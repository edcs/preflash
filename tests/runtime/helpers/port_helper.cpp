// Runs the virtual serial port in a process of its own, and plays the companion app against
// it. The argument picks a check, and what the check found goes to stdout.
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>

#include "esp_system.h"
#include "preflash/control.h"
#include "preflash/port.h"
#include "preflash/serial.h"

namespace port = preflash::port;

namespace {

using Clock = std::chrono::steady_clock;

std::string read_link(const std::string& path) {
  char target[1024];
  const ssize_t got = readlink(path.c_str(), target, sizeof target);
  return got < 0 ? "" : std::string(target, static_cast<size_t>(got));
}

int attach() {
  return open(port::path().c_str(), O_RDWR | O_NOCTTY);
}

void print_settings(int fd) {
  termios t{};
  tcgetattr(fd, &t);
  std::printf("echo=%s canonical=%s output=%s speed=%lu\n", (t.c_lflag & ECHO) ? "yes" : "no",
              (t.c_lflag & ICANON) ? "yes" : "no", (t.c_oflag & OPOST) ? "cooked" : "raw",
              static_cast<unsigned long>(cfgetospeed(&t)));
}

bool high_and_cloexec(int fd) {
  return fd > STDERR_FILENO && (fcntl(fd, F_GETFD) & FD_CLOEXEC);
}

int settings() {
  std::printf("path=%s\ndevice=%s\n", port::path().c_str(), port::device().c_str());
  const int fd = attach();
  if (fd < 0) return 1;
  print_settings(fd);
  struct stat info {};
  fstat(fd, &info);
  std::printf("mode=%o\n", info.st_mode & 0777);
  if (high_and_cloexec(port::master_fd()) && high_and_cloexec(port::slave_fd())) {
    std::printf("fds above stderr, closed on exec\n");
  }
  return 0;
}

int reopen() {
  close(attach());
  print_settings(attach());
  return 0;
}

int nobody() {
  const std::string chunk(4096, 'x');
  const auto start = Clock::now();
  for (int i = 0; i < 4096; ++i) port::write(chunk.data(), chunk.size());
  if (Clock::now() - start < std::chrono::seconds(1)) std::printf("wrote without waiting\n");
  const auto waited = Clock::now();
  port::wait_sent();
  if (Clock::now() - waited < std::chrono::milliseconds(300)) {
    std::printf("gave up on the queue quickly\n");
  }
  return 0;
}

// Reads from fd until it goes quiet, so that a test sees everything that's coming.
std::string read_all(int fd, int quiet_ms) {
  std::string out;
  char buffer[4096];
  for (;;) {
    pollfd ready = {fd, POLLIN, 0};
    if (poll(&ready, 1, quiet_ms) <= 0) return out;
    const ssize_t got = read(fd, buffer, sizeof buffer);
    if (got <= 0) return out;
    out.append(buffer, static_cast<size_t>(got));
  }
}

std::string numbered(int count) {
  std::string out;
  for (int i = 0; i < count; ++i) out += std::to_string(i) + "\n";
  return out;
}

int burst() {
  const int fd = attach();
  const std::string sent = numbered(4000);
  std::thread reader([&] {
    if (read_all(fd, 500) == sent) std::printf("burst arrived whole\n");
  });
  for (size_t at = 0; at < sent.size(); at += 512) {
    port::write(sent.data() + at, std::min<size_t>(512, sent.size() - at));
  }
  reader.join();
  return 0;
}

int slow() {
  const int fd = attach();
  const std::string sent = numbered(40000);
  for (size_t at = 0; at < sent.size(); at += 512) {
    port::write(sent.data() + at, std::min<size_t>(512, sent.size() - at));
  }
  const std::string got = read_all(fd, 300);
  const bool shorter = got.size() < sent.size();
  const bool newest = got.size() >= 16 && got.compare(got.size() - 16, 16, sent, sent.size() - 16, 16) == 0;
  if (shorter && newest) std::printf("kept the newest\n");
  return 0;
}

void add_print_control() {
  preflash::add_control("print", [](std::vector<std::string> words) {
    std::printf("control ran: %s\n", words.empty() ? "" : words[0].c_str());
    std::fflush(stdout);
  });
}

void send(int fd, const char* text) {
  if (::write(fd, text, std::strlen(text)) < 0) std::perror("write");
}

int input() {
  add_print_control();
  if (!preflash::start_console_reader()) return 1;
  const int fd = attach();
  send(fd, "@sim print one\n@sim print two\r\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  return 0;
}

// The first run leaves a line in the terminal while it restarts, and the second one must
// find the same device and that line.
int restart() {
  if (!std::getenv("PREFLASH_HELPER_DEVICE")) {
    setenv("PREFLASH_HELPER_DEVICE", port::device().c_str(), 1);
    port::hold_input();
    send(attach(), "@sim print waited\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    esp_restart();
  }
  if (port::device() == std::getenv("PREFLASH_HELPER_DEVICE")) {
    std::printf("same device after restart\n");
  }
  if (high_and_cloexec(port::master_fd()) && high_and_cloexec(port::slave_fd())) {
    std::printf("fds closed on exec\n");
  }
  add_print_control();
  if (!preflash::start_console_reader()) return 1;
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  return 0;
}

// A line too long to keep, with the stream running, so that any notice about it would
// reach a client and could reach the port.
int notice() {
  std::mutex mutex;
  std::string heard;
  preflash::serial::subscribe([&](const char* data, size_t size) {
    std::lock_guard<std::mutex> lock(mutex);
    heard.append(data, size);
  });
  const int fd = attach();
  const std::string line(1024 * 1024 + 16, 'x');
  std::thread writer([&] {
    if (::write(fd, line.data(), line.size()) < 0) std::perror("write");
    send(fd, "\n");
  });
  const std::string got = read_all(fd, 500);
  writer.join();
  bool clean;
  bool told;
  {
    std::lock_guard<std::mutex> lock(mutex);
    told = heard.find("too long") != std::string::npos;
    clean = got.find("too long") == std::string::npos;
  }
  if (told) std::printf("clients heard the notice\n");
  if (clean) std::printf("the port stayed clean\n");
  std::fflush(stdout);
  preflash::serial::release();
  return 0;
}

int links() {
  const char* extra = std::getenv("PREFLASH_PORT_LINK");
  if (read_link(extra) == port::device()) std::printf("extra points at the device\n");
  port::remove_links();
  if (read_link(extra).empty() && read_link(port::path()).empty()) std::printf("both gone\n");
  return 0;
}

// Starts threads until the system refuses, which macOS does per process.
void use_up_threads() {
  pthread_attr_t small;
  pthread_attr_init(&small);
  pthread_attr_setstacksize(&small, PTHREAD_STACK_MIN);
  pthread_t thread;
  const auto wait = [](void*) -> void* {
    pause();
    return nullptr;
  };
  while (pthread_create(&thread, &small, wait, nullptr) == 0) {
  }
}

int off() {
  if (port::path().empty()) std::printf("no port\n");
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) return 2;
  const std::string mode = argv[1];
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (mode == "notice" && !preflash::serial::start()) return 1;
  if (mode == "nothreads") {
    use_up_threads();
    port::start();
    return off();
  }
  port::start();
  if (mode == "off") return off();
  if (mode == "settings") return settings();
  if (mode == "reopen") return reopen();
  if (mode == "nobody") return nobody();
  if (mode == "burst") return burst();
  if (mode == "slow") return slow();
  if (mode == "input") return input();
  if (mode == "restart") return restart();
  if (mode == "links") return links();
  if (mode == "notice") return notice();
  return 2;
}
