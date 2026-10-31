#pragma once

// Stops the process, naming a call the simulator doesn't implement, so firmware
// never carries on as if the call had worked.
[[noreturn]] void preflash_unsupported(const char* call, const char* why);
