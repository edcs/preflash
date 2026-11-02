#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "esp_console.h"
#include "preflash/control.h"
#include "preflash/fail.h"
#include "preflash/socket.h"

namespace {

constexpr const char* kControlPrefix = "@sim ";
constexpr const char* kDefaultPrompt = "esp>";
// How long quit waits for the next queued line to finish before it stops waiting.
constexpr std::chrono::seconds kQuitPatience{1};
constexpr size_t kQueuedLineBytes = 1024 * 1024;

struct Command {
  std::string help;
  std::string hint;
  esp_console_cmd_func_t func;
  esp_console_cmd_func_with_context_t func_w_context;
  void* context;
};

struct Repl : esp_console_repl_t {
  std::string prompt;
  bool started = false;
};

struct State {
  std::mutex mutex;
  std::map<std::string, Command> commands;
  std::map<std::string, std::function<void(std::vector<std::string>)>> controls;
  // Set once firmware's REPL takes commands.
  const Repl* open_repl = nullptr;
  std::deque<std::string> lines;
  size_t queued_bytes = 0;
  std::condition_variable line_ready;
  bool line_running = false;
  uint64_t lines_finished = 0;
  std::condition_variable line_finished;
};

State& state() {
  static State* s = new State;  // never destroyed: the console task outlives static destructors
  return *s;
}

// Splits as ESP-IDF's esp_console_split_argv does. A quote opens a word only at
// its start, and its closing quote ends the word; if none closes it, the word
// runs to the end of the line. A backslash takes a following backslash, quote or
// space literally and drops anything else.
std::vector<std::string> split_words(const std::string& line) {
  enum class State { Space, Word, Quoted };
  std::vector<std::string> words;
  std::string word;
  State state = State::Space;
  bool escaped = false;
  const auto finish = [&] {
    words.push_back(word);
    word.clear();
    state = State::Space;
  };
  for (const char c : line) {
    if (escaped) {
      if (c == '\\' || c == '"' || c == ' ') word += c;
      escaped = false;
    } else if (state == State::Space) {
      if (c == ' ' || c == '\r' || c == '\n') continue;
      state = c == '"' ? State::Quoted : State::Word;
      if (c == '\\') {
        escaped = true;
      } else if (c != '"') {
        word += c;
      }
    } else if (c == '\\') {
      escaped = true;
    } else if (state == State::Quoted ? c == '"' : c == ' ' || c == '\r' || c == '\n') {
      finish();
    } else {
      word += c;
    }
  }
  if (state != State::Space) words.push_back(word);
  return words;
}

void run_control(std::vector<std::string> words) {
  if (words.empty()) {
    std::printf("@sim needs a control name\n");
    std::fflush(stdout);
    return;
  }
  std::function<void(std::vector<std::string>)> fn;
  {
    std::lock_guard<std::mutex> lock(state().mutex);
    auto it = state().controls.find(words[0]);
    if (it != state().controls.end()) fn = it->second;
  }
  if (!fn) {
    std::printf("Unknown control: %s\n", words[0].c_str());
    std::fflush(stdout);
    return;
  }
  words.erase(words.begin());
  fn(std::move(words));
}

void run_command(const std::vector<std::string>& words) {
  if (words.empty()) return;
  Command command{};
  {
    std::lock_guard<std::mutex> lock(state().mutex);
    auto it = state().commands.find(words[0]);
    if (it == state().commands.end()) {
      std::printf("Unrecognized command: %s\n", words[0].c_str());
      std::fflush(stdout);
      return;
    }
    command = it->second;
  }
  // argv ends with a null pointer, as a command expects.
  std::vector<std::string> storage = words;
  std::vector<char*> argv;
  for (auto& w : storage) argv.push_back(w.data());
  argv.push_back(nullptr);
  const int argc = static_cast<int>(words.size());
  const int code = command.func ? command.func(argc, argv.data())
                                : command.func_w_context(command.context, argc, argv.data());
  if (code != 0) {
    std::printf("Command returned non-zero error code: 0x%x\n", static_cast<unsigned>(code));
  }
  std::fflush(stdout);
}

bool is_control(const std::string& line) { return line.rfind(kControlPrefix, 0) == 0; }

// "@sim quit" must work even for firmware that never starts a REPL, so lines
// waiting for one can't hold it back.
bool is_quit(const std::string& line) {
  if (!is_control(line)) return false;
  const std::vector<std::string> words =
      split_words(line.substr(std::char_traits<char>::length(kControlPrefix)));
  return !words.empty() && words[0] == "quit";
}

// Counts the newline, so that empty lines can't fill memory for free.
size_t queued_size(const std::string& line) { return line.size() + 1; }

void print_prompt() {
  if (!isatty(STDIN_FILENO)) return;
  std::string prompt;
  {
    std::lock_guard<std::mutex> lock(state().mutex);
    if (!state().open_repl) return;
    prompt = state().open_repl->prompt;
  }
  std::printf("%s ", prompt.c_str());
  std::fflush(stdout);
}

std::atomic<bool> g_reader_done{false};

// Runs the queue. A control can go ahead of any REPL, so a project without one still
// answers it, but a firmware command waits for the REPL with every later line behind it,
// as it would in a device's UART buffer.
void line_task(void*) {
  State& s = state();
  for (;;) {
    std::string line;
    {
      std::unique_lock<std::mutex> lock(s.mutex);
      s.line_ready.wait(lock, [&] {
        return !s.lines.empty() && (s.open_repl || is_control(s.lines.front()));
      });
      line = std::move(s.lines.front());
      s.lines.pop_front();
      s.queued_bytes -= queued_size(line);
      s.line_running = true;
    }
    preflash::dispatch_line(line);
    print_prompt();
    {
      std::lock_guard<std::mutex> lock(s.mutex);
      s.line_running = false;
      ++s.lines_finished;
    }
    s.line_finished.notify_all();
  }
}

void reader_task(void*) {
  std::string line;
  // End of input stops the reading and nothing else: the firmware keeps running.
  while (std::getline(std::cin, line)) {
    if (!preflash::submit_line(line)) {
      std::printf("preflash: the console queue is full, so a line was dropped\n");
      std::fflush(stdout);
    }
  }
  g_reader_done = true;
}

// A device's REPL always has "help", so the simulator's does too.
int help_command(int, char**) {
  std::lock_guard<std::mutex> lock(state().mutex);
  for (const auto& [name, command] : state().commands) {
    std::printf("%-12s %s%s%s\n", name.c_str(), command.hint.c_str(), command.hint.empty() ? "" : "  ",
                command.help.c_str());
  }
  return 0;
}

esp_err_t repl_del(esp_console_repl_t*) {
  // Deleting a running reader would mean stopping a blocked read; say so.
  preflash_unsupported("esp_console_repl_del", "stopping the console reader is not simulated");
}

}  // namespace

namespace preflash {

void add_control(const char* name, std::function<void(std::vector<std::string>)> fn) {
  std::lock_guard<std::mutex> lock(state().mutex);
  state().controls[name] = std::move(fn);
}

void dispatch_line(const std::string& line) {
  if (is_control(line)) {
    run_control(split_words(line.substr(std::char_traits<char>::length(kControlPrefix))));
  } else {
    run_command(split_words(line));
  }
}

bool is_quit_line(const std::string& line) { return is_quit(line); }

void finish_queued_lines() {
  State& s = state();
  std::unique_lock<std::mutex> lock(s.mutex);
  // Only the lines here now, or a steady stream behind them could hold quit off for good.
  const uint64_t last = s.lines_finished + s.lines.size() + (s.line_running ? 1 : 0);
  while (s.lines_finished < last) {
    const uint64_t finished = s.lines_finished;
    const bool moved = s.line_finished.wait_for(
        lock, kQuitPatience, [&] { return s.lines_finished != finished; });
    if (!moved) return;
  }
}

bool submit_line(const std::string& line) {
  if (is_quit(line)) {
    socket::note_quitting();
    finish_queued_lines();
    dispatch_line(line);
    return true;
  }
  {
    std::lock_guard<std::mutex> lock(state().mutex);
    if (state().queued_bytes + queued_size(line) > kQueuedLineBytes) return false;
    state().queued_bytes += queued_size(line);
    state().lines.push_back(line);
  }
  state().line_ready.notify_one();
  return true;
}

bool console_reader_done() { return g_reader_done; }

bool start_console_reader() {
  static std::mutex mutex;
  static bool lines_started = false;
  static bool reader_started = false;
  std::lock_guard<std::mutex> lock(mutex);
  if (!lines_started) {
    lines_started = xTaskCreate(line_task, "console", 4096, nullptr, 2, nullptr) == pdPASS;
  }
  if (lines_started && !reader_started) {
    reader_started = xTaskCreate(reader_task, "stdin", 4096, nullptr, 2, nullptr) == pdPASS;
  }
  return reader_started;
}

}  // namespace preflash

extern "C" {

esp_err_t esp_console_cmd_register(const esp_console_cmd_t* cmd) {
  if (!cmd || !cmd->command || !*cmd->command) return ESP_ERR_INVALID_ARG;
  // A command has exactly one entry point.
  if ((cmd->func != nullptr) == (cmd->func_w_context != nullptr)) return ESP_ERR_INVALID_ARG;
  std::lock_guard<std::mutex> lock(state().mutex);
  state().commands[cmd->command] =
      Command{cmd->help ? cmd->help : "", cmd->hint ? cmd->hint : "", cmd->func,
              cmd->func_w_context, cmd->context};
  return ESP_OK;
}

esp_err_t esp_console_new_repl_usb_serial_jtag(const esp_console_dev_usb_serial_jtag_config_t*,
                                               const esp_console_repl_config_t* repl_config,
                                               esp_console_repl_t** ret_repl) {
  if (!repl_config || !ret_repl) return ESP_ERR_INVALID_ARG;
  {
    std::lock_guard<std::mutex> lock(state().mutex);
    if (!state().commands.count("help")) {
      state().commands["help"] = Command{"Print the list of registered commands", "", help_command,
                                         nullptr, nullptr};
    }
  }
  auto* repl = new Repl;
  repl->del = repl_del;
  repl->prompt = repl_config->prompt ? repl_config->prompt : kDefaultPrompt;
  *ret_repl = repl;
  return ESP_OK;
}

esp_err_t esp_console_start_repl(esp_console_repl_t* handle) {
  if (!handle) return ESP_ERR_INVALID_ARG;
  auto* repl = static_cast<Repl*>(handle);
  if (repl->started) return ESP_ERR_INVALID_STATE;
  repl->started = true;
  if (!preflash::start_console_reader()) {
    // Leave the repl startable so the caller can retry.
    repl->started = false;
    return ESP_FAIL;
  }
  {
    std::lock_guard<std::mutex> lock(state().mutex);
    state().open_repl = repl;
  }
  state().line_ready.notify_one();
  print_prompt();
  return ESP_OK;
}

}  // extern "C"
