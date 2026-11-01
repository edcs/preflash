#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

#include "esp_console.h"
#include "preflash/control.h"
#include "preflash/fail.h"

namespace {

constexpr const char* kControlPrefix = "@sim ";
constexpr const char* kDefaultPrompt = "esp>";

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
  // Set once firmware's REPL takes commands. Lines that arrive sooner wait for it,
  // as they would in a device's UART buffer.
  const Repl* open_repl = nullptr;
  std::deque<std::string> early_lines;
  // True while the REPL replays early_lines, so a line read meanwhile queues behind them.
  bool replaying = false;
};

State& state() {
  static State s;
  return s;
}

// Splits as ESP-IDF's esp_console_split_argv does: words end at a space, a
// backslash takes the next character literally, and a quote that opens a word
// holds it together up to the closing quote, or to the end of the line if none
// closes it.
std::vector<std::string> split_words(const std::string& line) {
  std::vector<std::string> words;
  std::string word;
  bool in_word = false;
  char quote = 0;
  for (size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\\' && i + 1 < line.size()) {
      word += line[++i];
      in_word = true;
    } else if (quote) {
      if (c == quote) {
        quote = 0;
      } else {
        word += c;
      }
    } else if (c == ' ' || c == '\r' || c == '\n') {
      if (in_word) words.push_back(word);
      word.clear();
      in_word = false;
    } else if (c == '"' && !in_word) {
      quote = c;
      in_word = true;
    } else {
      word += c;
      in_word = true;
    }
  }
  if (in_word) words.push_back(word);
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
  std::istringstream words(line.substr(std::string(kControlPrefix).size()));
  std::string name;
  return (words >> name) && name == "quit";
}

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

// Read from the start, whether or not firmware ever starts a REPL, so that a
// project without one still answers controls.
void reader_task(void*) {
  std::string line;
  // End of input stops the reading and nothing else: the firmware keeps running.
  while (std::getline(std::cin, line)) {
    {
      std::lock_guard<std::mutex> lock(state().mutex);
      // Once one line waits, every later line waits behind it, controls included,
      // except quit.
      if (!state().open_repl && !is_quit(line) && (state().replaying || !state().early_lines.empty() || !is_control(line))) {
        state().early_lines.push_back(line);
        continue;
      }
    }
    preflash::dispatch_line(line);
    print_prompt();
  }
  g_reader_done = true;
}

void repl_task(void* arg) {
  for (;;) {
    std::deque<std::string> lines;
    {
      std::lock_guard<std::mutex> lock(state().mutex);
      if (state().early_lines.empty()) {
        state().open_repl = static_cast<Repl*>(arg);
        state().replaying = false;
        break;
      }
      state().replaying = true;
      lines.swap(state().early_lines);
    }
    for (const std::string& line : lines) preflash::dispatch_line(line);
  }
  print_prompt();
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

bool console_reader_done() { return g_reader_done; }

bool start_console_reader() {
  static std::mutex mutex;
  static bool started = false;
  std::lock_guard<std::mutex> lock(mutex);
  if (!started) started = xTaskCreate(reader_task, "console", 4096, nullptr, 2, nullptr) == pdPASS;
  return started;
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
  if (!preflash::start_console_reader() ||
      xTaskCreate(repl_task, "repl", 4096, repl, 2, nullptr) != pdPASS) {
    // Leave the repl startable so the caller can retry.
    repl->started = false;
    return ESP_FAIL;
  }
  return ESP_OK;
}

}  // extern "C"
