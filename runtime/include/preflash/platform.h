#pragma once

#include <sys/types.h>

#include <cstddef>
#include <string>

// What macOS and Linux do differently, behind one name each.
namespace preflash::platform {

// The running executable's full path, or empty if the system won't say.
std::string executable_path();

// Gives back path without the " (deleted)" that Linux's /proc/self/exe adds once the file
// was replaced.
std::string without_deleted_mark(std::string path);

// The arguments the process started with, as execve takes them.
char** arguments();

// The process's environment, as execve takes it.
char** environment();

// Leaves only the descriptors in keep open across the next exec (the standard three are
// always kept). Returns false if it couldn't. macOS leaves this to posix_spawn's flags, so
// there it does nothing.
bool close_on_exec_except(const int* keep, int count);

// Marks every open descriptor above stderr close-on-exec by listing /proc/self/fd, for
// kernels or sandboxes without close_range. Returns false if it couldn't list them or mark
// one. Where /proc is missing, as on macOS, it always returns false.
bool close_on_exec_by_listing();

// The process at the other end of a connected local socket, as "pid N, name", or empty if the
// system won't say.
std::string peer(int fd);

// Keeps a hang-up from raising SIGPIPE at the process for writes to fd that go through
// write. macOS does that with a socket option; Linux has none, so send_some does it
// per call and this does nothing.
void ignore_hang_ups(int fd);

// write for a socket, with the same results and errno, that returns EPIPE for a client that
// has gone instead of raising SIGPIPE. Linux does that with a flag on each send.
ssize_t send_some(int fd, const void* data, size_t size);

}  // namespace preflash::platform
