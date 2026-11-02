#include "preflash/crash.h"

#include <fcntl.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "esp_timer.h"
#include "preflash/port.h"
#include "preflash/serial.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define PREFLASH_ASAN 1
#endif
#endif

// Everything from on_fatal_signal down runs in a signal handler, on a thread that could have
// stopped anywhere, so it takes no lock and allocates nothing. It calls what POSIX lists as
// async-signal-safe, and reads only atomics and what install() and note() wrote beforehand.
// Reports are built here by hand for that reason, and Go symbolises them afterwards.
namespace preflash::crash {
namespace {

constexpr int kFatalSignals[] = {SIGABRT, SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGTRAP};
// Sent to firmware's threads to stop them where they are. Firmware has no use for signals.
constexpr int kFreezeSignal = SIGUSR2;
constexpr int kFreezeWaitMs = 100;
constexpr int kMostThreads = 256;
constexpr int kMostFrames = 64;
constexpr size_t kNameBytes = 32;
constexpr size_t kKindBytes = 32;
constexpr size_t kMessageBytes = 1024;
constexpr size_t kAlternateStackBytes = 256 * 1024;
constexpr size_t kOutBytes = 4096;
constexpr size_t kLongestSanitizerText = 256 * 1024;
constexpr uint32_t kDataAbortLower = 0x24;
constexpr uint32_t kDataAbortSame = 0x25;
constexpr uint32_t kInstructionAbortLower = 0x20;
constexpr uint32_t kInstructionAbortSame = 0x21;
constexpr uint32_t kEsrClassShift = 26;
constexpr uint32_t kEsrClassMask = 0x3f;
constexpr uint32_t kEsrWrite = 1u << 6;
// System code's return addresses carry a pointer signature in their top bits.
constexpr uintptr_t kAddressBits = 0x00007fffffffffff;

static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<pthread_t>::is_always_lock_free);
static_assert(std::atomic<int>::is_always_lock_free);

struct Thread {
  std::atomic<pthread_t> owner{nullptr};
  char name[kNameBytes] = "";  // only its owner reads or writes this
  void* alternate_stack = nullptr;
};

std::atomic<bool> g_claimed{false};
std::atomic<int> g_frozen{0};
Thread g_threads[kMostThreads];

char g_file[PATH_MAX] = "";
char g_file_next[PATH_MAX] = "";
char g_sanitizer_log[PATH_MAX] = "";
bool g_hold = false;
char g_executable[PATH_MAX] = "";
char g_uuid[37] = "";
uintptr_t g_load_address = 0;
uint64_t g_image_size = 0;
int64_t g_boot_ns = 0;
int g_probe[2] = {-1, -1};

std::atomic<bool> g_note_taken{false};
std::atomic<pthread_t> g_note_owner{nullptr};
char g_note_kind[kKindBytes] = "";
char g_note_message[kMessageBytes] = "";

int64_t monotonic_ns() {
  timespec now{};
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec * 1000000000LL + now.tv_nsec;
}

void copy_text(char* to, size_t size, const char* from) {
  size_t i = 0;
  for (; i + 1 < size && from[i]; ++i) to[i] = from[i];
  to[i] = '\0';
}

void read_setting(const char* name, char* to, size_t size) {
  const char* value = std::getenv(name);
  if (value && std::strlen(value) < size) copy_text(to, size, value);
}

[[noreturn]] void park() {
  for (;;) pause();
}

// Takes a short line of text at a time, so that a report of any size fits this much stack.
class Out {
 public:
  explicit Out(int fd) : fd_(fd) {}

  void put(char c) {
    if (used_ == sizeof buffer_) flush();
    buffer_[used_++] = c;
  }

  void text(const char* s) {
    for (; *s; ++s) put(*s);
  }

  void escaped(const char* s, size_t size) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < size; ++i) {
      const auto c = static_cast<unsigned char>(s[i]);
      if (c == '"' || c == '\\') {
        put('\\');
        put(static_cast<char>(c));
      } else if (c == '\n') {
        text("\\n");
      } else if (c == '\t') {
        text("\\t");
      } else if (c < 0x20) {
        text("\\u00");
        put(kHex[c >> 4]);
        put(kHex[c & 15]);
      } else {
        put(static_cast<char>(c));
      }
    }
  }

  void number(int64_t value) {
    if (value < 0) {
      put('-');
      value = -value;
    }
    char digits[20];
    int count = 0;
    do {
      digits[count++] = static_cast<char>('0' + value % 10);
      value /= 10;
    } while (value > 0);
    while (count > 0) put(digits[--count]);
  }

  void hex(uint64_t value) {
    static const char kHex[] = "0123456789abcdef";
    text("0x");
    int shift = 60;
    while (shift > 0 && (value >> shift) == 0) shift -= 4;
    for (; shift >= 0; shift -= 4) put(kHex[(value >> shift) & 15]);
  }

  void field(const char* name) {
    text(first_ ? "{\"" : ",\"");
    first_ = false;
    text(name);
    text("\":");
  }

  void string_field(const char* name, const char* value) {
    field(name);
    put('"');
    escaped(value, std::strlen(value));
    put('"');
  }

  void hex_field(const char* name, uint64_t value) {
    field(name);
    put('"');
    hex(value);
    put('"');
  }

  void number_field(const char* name, int64_t value) {
    field(name);
    number(value);
  }

  void flush() {
    size_t at = 0;
    while (at < used_) {
      const ssize_t wrote = write(fd_, buffer_ + at, used_ - at);
      if (wrote <= 0) break;
      at += static_cast<size_t>(wrote);
    }
    used_ = 0;
  }

 private:
  int fd_;
  char buffer_[kOutBytes];
  size_t used_ = 0;
  bool first_ = true;
};

const char* signal_name(int number) {
  switch (number) {
    case SIGABRT: return "SIGABRT";
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGTRAP: return "SIGTRAP";
    default: return "signal";
  }
}

// What a fault was doing, from the exception the CPU took.
const char* access_of(int number, const ucontext_t* context) {
  if (number != SIGSEGV && number != SIGBUS) return "";
  const uint32_t esr = context->uc_mcontext->__es.__esr;
  const uint32_t exception_class = (esr >> kEsrClassShift) & kEsrClassMask;
  if (exception_class == kInstructionAbortLower || exception_class == kInstructionAbortSame) {
    return "execute";
  }
  if (exception_class == kDataAbortLower || exception_class == kDataAbortSame) {
    return (esr & kEsrWrite) ? "write" : "read";
  }
  return "";
}

// Copies memory that could be anywhere. The kernel checks a pipe write's buffer and fails it
// with EFAULT, where touching the address here would fault again.
bool copy_safely(uintptr_t from, void* to, size_t size) {
#ifdef PREFLASH_ASAN
  // ASan checks a write's buffer too, and would stop here with a report of its own.
  if (__asan_region_is_poisoned(reinterpret_cast<void*>(from), size)) return false;
#endif
  if (write(g_probe[1], reinterpret_cast<const void*>(from), size) != static_cast<ssize_t>(size)) {
    return false;
  }
  return read(g_probe[0], to, size) == static_cast<ssize_t>(size);
}

int walk_frames(const ucontext_t* context, uintptr_t* frames) {
  int count = 0;
  frames[count++] = context->uc_mcontext->__ss.__pc & kAddressBits;
  uintptr_t frame = context->uc_mcontext->__ss.__fp;
  while (count < kMostFrames && frame != 0 && (frame & 7) == 0) {
    uintptr_t record[2];  // the caller's frame, then the return address
    if (!copy_safely(frame, record, sizeof record)) break;
    const uintptr_t back = record[1] & kAddressBits;
    if (back == 0) break;
    frames[count++] = back;
    if (record[0] <= frame) break;
    frame = record[0];
  }
  return count;
}

void add_file_text(Out& out, const char* path) {
  const int in = open(path, O_RDONLY | O_CLOEXEC);
  if (in < 0) return;
  char chunk[1024];
  size_t total = 0;
  ssize_t got;
  while (total < kLongestSanitizerText && (got = read(in, chunk, sizeof chunk)) > 0) {
    out.escaped(chunk, static_cast<size_t>(got));
    total += static_cast<size_t>(got);
  }
  close(in);
}

void show_file(const char* path) {
  const int in = open(path, O_RDONLY | O_CLOEXEC);
  if (in < 0) return;
  Out err(STDERR_FILENO);
  char chunk[1024];
  size_t total = 0;
  ssize_t got;
  while (total < kLongestSanitizerText && (got = read(in, chunk, sizeof chunk)) > 0) {
    for (ssize_t i = 0; i < got; ++i) err.put(chunk[i]);
    total += static_cast<size_t>(got);
  }
  err.flush();
  close(in);
}

// A sanitizer names its log after the process.
void sanitizer_path(char* to, size_t size) {
  to[0] = '\0';
  if (!g_sanitizer_log[0]) return;
  const size_t length = std::strlen(g_sanitizer_log);
  if (length + 22 > size) return;
  int64_t pid = getpid();
  char digits[20];
  int count = 0;
  do {
    digits[count++] = static_cast<char>('0' + pid % 10);
    pid /= 10;
  } while (pid > 0);
  std::memcpy(to, g_sanitizer_log, length);
  size_t at = length;
  to[at++] = '.';
  while (count > 0) to[at++] = digits[--count];
  to[at] = '\0';
}

const char* task_name() {
  const pthread_t self = pthread_self();
  for (const Thread& thread : g_threads) {
    if (thread.owner.load(std::memory_order_relaxed) == self) return thread.name;
  }
  return "";
}

void write_report(int number, const siginfo_t* info, const ucontext_t* context, const char* log) {
  const int fd = open(g_file_next, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return;
  uintptr_t frames[kMostFrames];
  const int count = walk_frames(context, frames);
  const bool noted = g_note_owner.load(std::memory_order_acquire) == pthread_self();
  timespec now{};
  clock_gettime(CLOCK_REALTIME, &now);

  Out out(fd);
  out.number_field("version", 1);
  out.number_field("pid", getpid());
  out.number_field("time", now.tv_sec);
  out.number_field("uptime_ms", (monotonic_ns() - g_boot_ns) / 1000000);
  out.string_field("signal", signal_name(number));
  out.number_field("code", info->si_code);
  out.hex_field("address", reinterpret_cast<uintptr_t>(info->si_addr));
  out.string_field("access", access_of(number, context));
  out.string_field("task", task_name());
  out.string_field("kind", noted ? g_note_kind : "");
  out.string_field("message", noted ? g_note_message : "");
  out.string_field("executable", g_executable);
  out.string_field("uuid", g_uuid);
  out.hex_field("load_address", g_load_address);
  out.number_field("image_size", static_cast<int64_t>(g_image_size));
  out.hex_field("lr", context->uc_mcontext->__ss.__lr & kAddressBits);
  out.field("frames");
  out.put('[');
  for (int i = 0; i < count; ++i) {
    if (i > 0) out.put(',');
    out.put('"');
    out.hex(frames[i]);
    out.put('"');
  }
  out.put(']');
  if (log[0]) {
    out.field("sanitizer");
    out.put('"');
    add_file_text(out, log);
    out.put('"');
  }
  out.text("}\n");
  out.flush();
  close(fd);
  rename(g_file_next, g_file);
}

void freeze_firmware() {
  const pthread_t self = pthread_self();
  int sent = 0;
  for (const Thread& thread : g_threads) {
    const pthread_t owner = thread.owner.load(std::memory_order_acquire);
    if (owner && owner != self && pthread_kill(owner, kFreezeSignal) == 0) ++sent;
  }
  for (int ms = 0; ms < kFreezeWaitMs && g_frozen.load() < sent; ++ms) {
    const timespec wait = {0, 1000000};
    nanosleep(&wait, nullptr);
  }
}

void say_crashed(int number) {
  Out err(STDERR_FILENO);
  const char* task = task_name();
  err.text("preflash: crashed on ");
  err.text(task[0] ? task : "a host thread");
  err.text(": ");
  err.text(signal_name(number));
  err.text(". preflash crash shows the report.\n");
  err.flush();
}

void on_freeze(int) {
  ++g_frozen;
  park();
}

void on_fatal_signal(int number, siginfo_t* info, void* raw_context) {
  // Whichever thread comes second stays where it is, for the first to finish.
  if (g_claimed.exchange(true)) park();
  const auto* context = static_cast<const ucontext_t*>(raw_context);
  freeze_firmware();
  if (g_file[0]) {
    char log[PATH_MAX + 32];
    sanitizer_path(log, sizeof log);
    write_report(number, info, context, log);
    if (log[0]) {
      show_file(log);
      unlink(log);
    }
    say_crashed(number);
  }
  // A process dying with output still in the pipe would lose the message that says why.
  serial::release();
  if (g_hold) {
    port::hang_up();
    park();
  }
  signal(number, SIG_DFL);
  raise(number);
}

void* new_alternate_stack() {
  void* stack = mmap(nullptr, kAlternateStackBytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
  return stack == MAP_FAILED ? nullptr : stack;
}

void use_alternate_stack(void* stack) {
  if (!stack) return;
  stack_t alternate{};
  alternate.ss_sp = stack;
  alternate.ss_size = kAlternateStackBytes;
  sigaltstack(&alternate, nullptr);
}

void read_image() {
  uint32_t size = sizeof g_executable;
  if (_NSGetExecutablePath(g_executable, &size) != 0) g_executable[0] = '\0';
  const auto* header = reinterpret_cast<const mach_header_64*>(_dyld_get_image_header(0));
  if (!header) return;
  g_load_address = reinterpret_cast<uintptr_t>(header);
  const auto* at = reinterpret_cast<const char*>(header + 1);
  for (uint32_t i = 0; i < header->ncmds; ++i) {
    const auto* command = reinterpret_cast<const load_command*>(at);
    if (command->cmd == LC_UUID) {
      const uint8_t* id = reinterpret_cast<const uuid_command*>(command)->uuid;
      std::snprintf(g_uuid, sizeof g_uuid,
                    "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", id[0],
                    id[1], id[2], id[3], id[4], id[5], id[6], id[7], id[8], id[9], id[10], id[11],
                    id[12], id[13], id[14], id[15]);
    } else if (command->cmd == LC_SEGMENT_64) {
      const auto* segment = reinterpret_cast<const segment_command_64*>(command);
      if (std::strcmp(segment->segname, SEG_TEXT) == 0) g_image_size = segment->vmsize;
    }
    at += command->cmdsize;
  }
}

}  // namespace

void install() {
  g_boot_ns = monotonic_ns() - esp_timer_get_time() * 1000;
  read_setting("PREFLASH_CRASH_FILE", g_file, sizeof g_file);
  read_setting("PREFLASH_SANITIZER_LOG", g_sanitizer_log, sizeof g_sanitizer_log);
  if (std::strlen(g_file) + sizeof ".next" <= sizeof g_file_next) {
    std::snprintf(g_file_next, sizeof g_file_next, "%s.next", g_file);
  } else {
    g_file[0] = '\0';
  }
  const char* hold = std::getenv("PREFLASH_CRASH_HOLD");
  g_hold = hold && std::strcmp(hold, "1") == 0;
  read_image();
  if (pipe(g_probe) == 0) {
    for (const int fd : g_probe) {
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      fcntl(fd, F_SETFL, O_NONBLOCK);
    }
  }
  use_alternate_stack(new_alternate_stack());

  struct sigaction fatal {};
  fatal.sa_sigaction = on_fatal_signal;
  fatal.sa_flags = SA_SIGINFO | SA_ONSTACK;
  sigfillset(&fatal.sa_mask);
  for (const int number : kFatalSignals) sigaction(number, &fatal, nullptr);

  struct sigaction freeze {};
  freeze.sa_handler = on_freeze;
  freeze.sa_flags = SA_ONSTACK;
  sigfillset(&freeze.sa_mask);
  sigaction(kFreezeSignal, &freeze, nullptr);
}

void enter_task(const char* name) {
  const pthread_t self = pthread_self();
  for (Thread& thread : g_threads) {
    pthread_t empty = nullptr;
    if (thread.owner.compare_exchange_strong(empty, self, std::memory_order_acq_rel)) {
      copy_text(thread.name, sizeof thread.name, name);
      if (!thread.alternate_stack) thread.alternate_stack = new_alternate_stack();
      use_alternate_stack(thread.alternate_stack);
      return;
    }
  }
  // Past the limit a thread goes without: it can't report an overflow, or be frozen.
}

void leave_task() {
  const pthread_t self = pthread_self();
  for (Thread& thread : g_threads) {
    if (thread.owner.load(std::memory_order_relaxed) != self) continue;
    stack_t none{};
    none.ss_flags = SS_DISABLE;
    sigaltstack(&none, nullptr);
    thread.owner.store(nullptr, std::memory_order_release);
    return;
  }
}

void note(const char* kind, const char* message) {
  if (g_note_taken.exchange(true)) return;
  copy_text(g_note_kind, sizeof g_note_kind, kind);
  copy_text(g_note_message, sizeof g_note_message, message);
  g_note_owner.store(pthread_self(), std::memory_order_release);
}

bool happened() { return g_claimed.load(std::memory_order_relaxed); }

}  // namespace preflash::crash
