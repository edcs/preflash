#include "link.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "winlink/protocol.h"

namespace preflash::window {
namespace {

// A line longer than this isn't a message, and an app that backs up more than this isn't reading.
constexpr size_t kLongestLine = 1 << 20;
constexpr size_t kLongestBacklog = 1 << 20;

void set_nonblocking(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  const int on = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof on);
#endif
}

}  // namespace

Link::~Link() {
  drop_client();
  if (listener_ >= 0) {
    close(listener_);
    unlink(path_.c_str());
  }
  // The lock file stays: removing a file someone may be about to lock would let two windows hold it.
  if (lock_ >= 0) close(lock_);
}

bool Link::listen(const std::string& path, std::string* error) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.empty() || path.size() >= sizeof addr.sun_path) {
    *error = "the socket's path is empty or too long: " + path;
    return false;
  }
  std::strncpy(addr.sun_path, path.c_str(), sizeof addr.sun_path - 1);

  // Whoever holds the lock owns the path, so a socket file found under it is stale. Asking the
  // file's listener instead would make it take us for its app.
  const int lock = open((path + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (lock < 0) {
    *error = "couldn't lock " + path + ": " + std::strerror(errno);
    return false;
  }
  if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
    *error = "another window already holds the socket at " + path;
    close(lock);
    return false;
  }
  struct stat info {};
  if (lstat(path.c_str(), &info) == 0) {
    if (!S_ISSOCK(info.st_mode)) {
      *error = path + " is in the way of the socket, and isn't one";
      close(lock);
      return false;
    }
    unlink(path.c_str());
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = std::string("no socket: ") + std::strerror(errno);
    close(lock);
    return false;
  }
  // Only the user may connect.
  const mode_t mask = umask(0177);
  const bool bound = bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0;
  const int bind_errno = errno;
  umask(mask);
  if (!bound || ::listen(fd, 4) != 0) {
    *error = "couldn't listen at " + path + ": " + std::strerror(bound ? errno : bind_errno);
    close(fd);
    close(lock);
    return false;
  }
  set_nonblocking(fd);
  if (listener_ >= 0) {
    close(listener_);
    unlink(path_.c_str());
  }
  if (lock_ >= 0) close(lock_);
  listener_ = fd;
  lock_ = lock;
  path_ = path;
  last_connected_ = std::chrono::steady_clock::now();
  return true;
}

void Link::drop_client() {
  if (client_ >= 0) {
    close(client_);
    client_ = -1;
    last_connected_ = std::chrono::steady_clock::now();
  }
  in_.clear();
  out_.clear();
}

std::chrono::steady_clock::time_point Link::last_connected() const {
  return client_ >= 0 ? std::chrono::steady_clock::now() : last_connected_;
}

void Link::poll(std::function<void(const nlohmann::json&)> on_message) {
  if (listener_ < 0) return;
  // The newest app wins: an older one is a leftover that its restart hasn't closed yet.
  for (;;) {
    const int fd = accept(listener_, nullptr, nullptr);
    if (fd < 0) break;
    drop_client();
    set_nonblocking(fd);
    client_ = fd;
    ++connections_;
  }
  if (client_ < 0) return;
  flush();
  if (client_ < 0) return;

  bool hung_up = false;
  char chunk[4096];
  for (;;) {
    const ssize_t got = read(client_, chunk, sizeof chunk);
    if (got > 0) {
      in_.append(chunk, static_cast<size_t>(got));
      continue;
    }
    if (got < 0 && errno == EINTR) continue;
    hung_up = got == 0 || (errno != EAGAIN && errno != EWOULDBLOCK);
    break;
  }

  // Lines the app finished before it hung up still count.
  std::string lines = std::move(in_);
  in_.clear();
  const size_t end = lines.rfind('\n');
  if (end == std::string::npos) {
    if (!hung_up && lines.size() <= kLongestLine) in_ = std::move(lines);
  } else {
    if (!hung_up) in_ = lines.substr(end + 1);
    lines.resize(end + 1);
    size_t start = 0;
    for (size_t nl; (nl = lines.find('\n', start)) != std::string::npos; start = nl + 1) {
      if (const auto message = winlink::decode_line(lines.substr(start, nl - start))) {
        on_message(*message);
      }
    }
  }
  if (hung_up) drop_client();
}

bool Link::flush() {
  while (client_ >= 0 && !out_.empty()) {
    const ssize_t sent = write(client_, out_.data(), out_.size());
    if (sent > 0) {
      out_.erase(0, static_cast<size_t>(sent));
      continue;
    }
    if (sent < 0 && errno == EINTR) continue;
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
    drop_client();
    return false;
  }
  return true;
}

bool Link::send(const nlohmann::json& message) {
  if (client_ < 0) return false;
  out_ += winlink::encode(message);
  if (!flush()) return false;
  if (out_.size() > kLongestBacklog) {
    drop_client();
    return false;
  }
  return true;
}

}  // namespace preflash::window
