// Restarts once, leaving a marker in PREFLASH_STATE; the second run reports it.
// With PREFLASH_HELPER_LEAK set, the first run also leaves a descriptor open and
// a signal blocked and another ignored, and the second run reports whether either survived.
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "esp_system.h"

int main() {
  const char* state = std::getenv("PREFLASH_STATE");
  if (!state) return 2;
  const bool leak = std::getenv("PREFLASH_HELPER_LEAK") != nullptr;
  const std::string marker = std::string(state) + "/restarted";
  if (FILE* seen = std::fopen(marker.c_str(), "r")) {
    std::fclose(seen);
    std::printf("second run: state=%s reason=%d\n", state, static_cast<int>(esp_reset_reason()));
    if (leak) {
      const char* fd_text = std::getenv("PREFLASH_HELPER_FD");
      const int fd = fd_text ? std::atoi(fd_text) : -1;
      const bool open = fd >= 0 && fcntl(fd, F_GETFD) != -1;
      sigset_t mask;
      pthread_sigmask(SIG_SETMASK, nullptr, &mask);
      struct sigaction action {};
      sigaction(SIGUSR2, nullptr, &action);
      std::printf("descriptor=%s signals=%s ignored=%s\n", open ? "open" : "closed",
                  sigismember(&mask, SIGUSR1) ? "blocked" : "unblocked",
                  action.sa_handler == SIG_IGN ? "yes" : "no");
    }
    return 0;
  }
  if (std::getenv("PREFLASH_HELPER_CLOSE_STDIN")) close(STDIN_FILENO);
  FILE* made = std::fopen(marker.c_str(), "w");
  if (!made) return 3;
  if (leak) {
    // Left open on purpose: the restart must not carry it over.
    setenv("PREFLASH_HELPER_FD", std::to_string(fileno(made)).c_str(), 1);
    sigset_t block;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    pthread_sigmask(SIG_BLOCK, &block, nullptr);
    signal(SIGUSR2, SIG_IGN);
  } else {
    std::fclose(made);
  }
  esp_restart();
}
