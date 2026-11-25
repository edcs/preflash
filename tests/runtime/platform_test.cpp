#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <string>

#include "preflash/platform.h"
#include "preflash/power.h"

namespace platform = preflash::platform;

extern char** environ;

namespace {

bool ends_with(const std::string& text, const std::string& end) {
  return text.size() >= end.size() && text.compare(text.size() - end.size(), end.size(), end) == 0;
}

// Runs check in a child and gives back its exit status, so that what it changes in the
// process stays there.
int in_child(bool (*check)()) {
  const pid_t pid = fork();
  if (pid == 0) _exit(check() ? 0 : 1);
  int status = 0;
  waitpid(pid, &status, 0);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// What /proc says of a fresh run of sh that exec_clean started from a child with SIGHUP and
// SIGUSR1 ignored and SIGUSR2 blocked, as the SigBlk and SigIgn masks.
struct Masks {
  uint64_t blocked = ~0ull;
  uint64_t ignored = 0;
};

Masks masks_after_exec_clean() {
  int out[2];
  if (pipe(out) != 0) return {};
  const pid_t pid = fork();
  if (pid == 0) {
    dup2(out[1], STDOUT_FILENO);
    signal(SIGHUP, SIG_IGN);
    signal(SIGUSR1, SIG_IGN);
    sigset_t usr2;
    sigemptyset(&usr2);
    sigaddset(&usr2, SIGUSR2);
    pthread_sigmask(SIG_BLOCK, &usr2, nullptr);
    char* const argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"),
                          const_cast<char*>("grep -E '^Sig(Ign|Blk)' /proc/self/status"), nullptr};
    preflash::exec_clean("/bin/sh", argv, environ);
    _exit(127);
  }
  close(out[1]);
  std::string text;
  char buffer[256];
  ssize_t got;
  while ((got = read(out[0], buffer, sizeof buffer)) > 0) text.append(buffer, got);
  close(out[0]);
  waitpid(pid, nullptr, 0);

  Masks masks;
  std::istringstream lines(text);
  std::string line;
  while (std::getline(lines, line)) {
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const uint64_t value = std::strtoull(line.c_str() + colon + 1, nullptr, 16);
    if (line.starts_with("SigBlk")) masks.blocked = value;
    if (line.starts_with("SigIgn")) masks.ignored = value;
  }
  return masks;
}

constexpr uint64_t bit(int number) { return 1ull << (number - 1); }

}  // namespace

TEST(Platform, ExecutablePathIsThisTestBinary) {
  const std::string path = platform::executable_path();
  EXPECT_TRUE(ends_with(path, "/runtime_tests")) << path;
  EXPECT_EQ(access(path.c_str(), X_OK), 0) << path;
}

TEST(Platform, ArgumentsMatchArgv) {
  char** arguments = platform::arguments();
  ASSERT_NE(arguments, nullptr);
  ASSERT_NE(arguments[0], nullptr);
  EXPECT_TRUE(ends_with(arguments[0], "runtime_tests")) << arguments[0];
  int count = 0;
  while (count < 1000 && arguments[count]) ++count;
  EXPECT_LT(count, 1000) << "no nullptr ends the arguments";
}

TEST(Platform, CloseOnExecExceptKeepsOnlyThoseNamed) {
#ifdef __APPLE__
  GTEST_SKIP() << "macOS leaves this to posix_spawn's flags";
#else
  EXPECT_EQ(in_child([] {
              int a[2];
              int b[2];
              if (pipe(a) != 0 || pipe(b) != 0) return false;
              if (!platform::close_on_exec_except(&a[0], 1)) return false;
              return (fcntl(a[0], F_GETFD) & FD_CLOEXEC) == 0 &&
                     (fcntl(b[0], F_GETFD) & FD_CLOEXEC) != 0;
            }),
            0);
#endif
}

TEST(Exec, KeepsNohupAcrossRestart) {
#ifdef __APPLE__
  GTEST_SKIP() << "reads the signal masks from /proc";
#else
  const Masks masks = masks_after_exec_clean();
  EXPECT_NE(masks.ignored & bit(SIGHUP), 0u);
  EXPECT_EQ(masks.ignored & bit(SIGUSR1), 0u);
#endif
}

TEST(Exec, StartsWithAnEmptyMask) {
#ifdef __APPLE__
  GTEST_SKIP() << "reads the signal masks from /proc";
#else
  EXPECT_EQ(masks_after_exec_clean().blocked, 0u);
#endif
}
