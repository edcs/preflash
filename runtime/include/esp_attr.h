#pragma once

// Where the linker puts code and data on the chip, which means nothing on the host.
// RTC memory keeps its contents through a restart and isn't modelled, so its attributes
// stay undefined and a project using them stops at the build.
#define IRAM_ATTR
#define DRAM_ATTR
#define EXT_RAM_BSS_ATTR
