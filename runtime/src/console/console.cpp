#include <unistd.h>

#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "esp_console.h"
#include "preflash/control.h"

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

struct State {
  std::mutex mutex;
  std::map<std::string, Command> commands;
  std::map<std::string, std::function<void(std::vector<std::string>)>> controls;
};

State& state() {
  static State s;
  return s;
}

// Splits as ESP-IDF's esp_console_split_argv does: words end at a space, a
// backslash takes the next character literally, and a quote that opens a word
// holds it together up to the closing quote.
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

struct Repl : esp_console_repl_t {
  std::string prompt;
  bool started = false;
};

void repl_task(void* arg) {
  const auto* repl = static_cast<Repl*>(arg);
  const bool interactive = isatty(STDIN_FILENO);
  std::string line;
  for (;;) {
    if (interactive) {
      std::printf("%s ", repl->prompt.c_str());
      std::fflush(stdout);
    }
    // End of input stops the reading and nothing else: the firmware keeps running.
    if (!std::getline(std::cin, line)) return;
    preflash::dispatch_line(line);
  }
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
  return ESP_ERR_NOT_SUPPORTED;
}

}  // namespace

namespace preflash {

void add_control(const char* name, std::function<void(std::vector<std::string>)> fn) {
  std::lock_guard<std::mutex> lock(state().mutex);
  state().controls[name] = std::move(fn);
}

void dispatch_line(const std::string& line) {
  const std::string prefix = kControlPrefix;
  if (line.compare(0, prefix.size(), prefix) == 0) {
    run_control(split_words(line.substr(prefix.size())));
  } else {
    run_command(split_words(line));
  }
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
  if (xTaskCreate(repl_task, "console", 4096, repl, 2, nullptr) != pdPASS) {
    // Leave the repl startable so the caller can retry.
    repl->started = false;
    return ESP_FAIL;
  }
  return ESP_OK;
}

}  // extern "C"
