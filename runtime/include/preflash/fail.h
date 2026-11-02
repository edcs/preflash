#pragma once

// Stops the process, naming a call the simulator doesn't implement, so firmware
// never carries on as if the call had worked.
[[noreturn]] void preflash_unsupported(const char* call, const char* why);

namespace preflash {

// Called in place of stopping the process, for a tool that reports a refusal and goes on. It must
// throw. Null puts the usual stop back.
using UnsupportedHandler = void (*)(const char* call, const char* why);
void set_unsupported_handler(UnsupportedHandler handler);

}  // namespace preflash
