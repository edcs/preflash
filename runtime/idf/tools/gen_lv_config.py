#!/usr/bin/env python3
"""Turns a project's sdkconfig into the LVGL config for the native build, so
Preflash renders with the same LVGL options as the firmware."""
import pathlib
import re
import sys

src, dst = sys.argv[1:3]
sanitizer = sys.argv[3] if len(sys.argv) > 3 else ""

# SDL belongs to the main thread alone, so a project's sdkconfig must not switch on LVGL's
# SDL driver. The host has no FreeRTOS for LVGL to lock with, but firmware that picked it
# relies on lv_lock and on lv_timer_handler taking that lock, so pthreads do the same job.
# Their stacks are the host's size, not the chip's.
skip = re.compile(
    r"CONFIG_LV_(OS_|USE_FREERTOS|USE_OS|USE_SDL|SDL_|USE_DRAW_SDL|DRAW_THREAD_STACK_SIZE)"
)
HOST_DRAW_STACK = 512 * 1024

# LVGL's own allocator hands out a single pool, where a sanitizer sees one big block and no
# overflow inside it. Under a sanitizer it uses malloc, whatever the project chose, and libc's
# string calls, which a sanitizer checks.
own_libraries = re.compile(r"CONFIG_LV_USE_[A-Z]+_(MALLOC|STRING|SPRINTF)$")

lines = ["#pragma once", "// Generated from " + src]
threads = False
for line in open(src):
    m = re.match(r"(CONFIG_LV[A-Z0-9_]*)=(.*)", line.strip())
    if m and m[1] == "CONFIG_LV_OS_FREERTOS" and m[2] == "y":
        threads = True
    if not m or skip.match(m[1]) or m[2] == "n":
        continue
    if sanitizer and own_libraries.match(m[1]):
        continue
    lines.append(f"#define {m[1]} {'1' if m[2] == 'y' else m[2]}")
if threads:
    lines.append("#define CONFIG_LV_OS_PTHREAD 1")
    lines.append(f"#define CONFIG_LV_DRAW_THREAD_STACK_SIZE {HOST_DRAW_STACK}")
else:
    lines.append("#define CONFIG_LV_OS_NONE 1")
if sanitizer:
    lines += [f"#define CONFIG_LV_USE_CLIB_{part} 1" for part in ("MALLOC", "STRING", "SPRINTF")]
text = "\n".join(lines) + "\n"

# LVGL is large, so leave the file alone unless the config really changed.
out = pathlib.Path(dst)
if not out.exists() or out.read_text() != text:
    out.write_text(text)
