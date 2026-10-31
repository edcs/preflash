#!/usr/bin/env python3
"""Turns a project's sdkconfig into the LVGL config for the native build, so
Preflash renders with the same LVGL options as the firmware."""
import pathlib
import re
import sys

src, dst = sys.argv[1:3]

# The host has no FreeRTOS for LVGL to lock with, and SDL belongs to the main
# thread alone, so a project's sdkconfig must not switch on LVGL's SDL driver.
skip = re.compile(r"CONFIG_LV_(OS_|USE_FREERTOS|USE_OS|USE_SDL|SDL_|USE_DRAW_SDL)")

lines = ["#pragma once", "// Generated from " + src]
for line in open(src):
    m = re.match(r"(CONFIG_LV[A-Z0-9_]*)=(.*)", line.strip())
    if not m or skip.match(m[1]) or m[2] == "n":
        continue
    lines.append(f"#define {m[1]} {'1' if m[2] == 'y' else m[2]}")
lines.append("#define CONFIG_LV_OS_NONE 1")
text = "\n".join(lines) + "\n"

# LVGL is large, so leave the file alone unless the config really changed.
out = pathlib.Path(dst)
if not out.exists() or out.read_text() != text:
    out.write_text(text)
